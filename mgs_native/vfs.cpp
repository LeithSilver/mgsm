#include "vfs.h"
#include "settings.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

namespace {
// A read-only stream over a preloaded file (shared, not copied).
class RamBuf : public std::streambuf {
public:
    explicit RamBuf(std::shared_ptr<const std::string> data) : data_(std::move(data)) {
        char* b = const_cast<char*>(data_->data());
        setg(b, b, b + data_->size());
    }
protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
        const off_type base = dir == std::ios_base::beg ? 0 : dir == std::ios_base::cur ? gptr() - eback() : egptr() - eback();
        return seekpos(pos_type(base + off), std::ios_base::in);
    }
    pos_type seekpos(pos_type pos, std::ios_base::openmode) override {
        if (pos < 0 || pos > egptr() - eback()) return pos_type(off_type(-1));
        setg(eback(), eback() + off_type(pos), egptr());
        return pos;
    }
private:
    std::shared_ptr<const std::string> data_;
};
class RamStream : public std::iostream {
public:
    explicit RamStream(std::shared_ptr<const std::string> data) : std::iostream(nullptr), buf_(std::move(data)) { rdbuf(&buf_); }
private:
    RamBuf buf_;
};
}

VirtualFileSystem& VirtualFileSystem::Instance() {
    static VirtualFileSystem instance;
    return instance;
}

void VirtualFileSystem::SetAssetRoot(const std::string& root) {
    asset_root_ = root;
    nocase_.clear();
    nocase_built_ = false;
    std::cout << "[VFS] Asset root set to: " << asset_root_ << std::endl;
}

std::string VirtualFileSystem::NormalisePath(const std::string& rel_path) const {
    std::string p = rel_path;
    std::replace(p.begin(), p.end(), '\\', '/');

    // Strip URL prefixes
    const std::vector<std::string> prefixes = {"rom:/", "raw:/", "ram:/", "data:/"};
    for (const auto& pre : prefixes) {
        if (p.rfind(pre, 0) == 0) {
            p = p.substr(pre.length());
            break;
        }
    }

    // Strip leading slashes
    while (!p.empty() && p.front() == '/') {
        p.erase(0, 1);
    }
    // One slash between parts: the game asks for "common/items//cctvCamera.group.bin" (Area 1). Windows and
    // Linux accept "//", the 3DS SD card doesn't, and the missing group sends the game's resource lookup
    // (0x4A0BE5DC) into endless recursion.
    p.erase(std::unique(p.begin(), p.end(), [](char a, char b) { return a == '/' && b == '/'; }), p.end());
    // "." and ".." resolved here: the game asks for "localisation/TextID_Lv1/../TextGlobal/english.dat", and the
    // RAM index (PreloadAll) has only real paths.
    if (p.find("./") != std::string::npos || p.find("/.") != std::string::npos) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find('/', start);
            if (end == std::string::npos) end = p.size();
            const std::string part = p.substr(start, end - start);
            if (part == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (part != "." && !part.empty()) parts.push_back(part);
            start = end + 1;
        }
        std::string out;
        for (const std::string& part : parts) out += (out.empty() ? "" : "/") + part;
        p = out;
    }
    return p;
}

std::string VirtualFileSystem::ResolvePath(const std::string& rel_path) {
    std::string p = NormalisePath(rel_path);
    if (preloaded_) {   // every file is in the index (FindNoCase): no disk access, misses included
        if (std::string real = FindNoCase(p); !real.empty()) return real;
        if (std::string real = FindNoCase(p + ".bin"); !real.empty()) return real;
        // ...except the .dz archives beside the data folder (below): their existence changes how the game
        // loads groups (without all.dz it takes another path that then fails, "Class factory does not
        // recognise hash 0x0"). Rare, so asked of the disk.
        if (p.size() <= 3 || p.compare(p.size() - 3, 3, ".dz") != 0) return "";
    }

    const fs::path base = fs::u8path(asset_root_);
    fs::path cand1 = base / fs::u8path(p);
    if (fs::is_regular_file(cand1)) {
        return cand1.u8string();
    }

    fs::path cand2 = base / fs::u8path(p + ".bin");
    if (fs::is_regular_file(cand2)) {
        return cand2.u8string();
    }

    // Same name with other capitals (case-sensitive file systems)
    if (std::string real = FindNoCase(p); !real.empty()) return real;
    if (std::string real = FindNoCase(p + ".bin"); !real.empty()) return real;

    // Derbh archives (all.dz, warp.dz) sit next to the extracted folder on the device
    // (Private/20010925/all.dz beside .../all/). The game only checks they exist; see the
    // dzArchiveAttach HLE in runtime.cpp.
    if (p.size() > 3 && p.compare(p.size() - 3, 3, ".dz") == 0) {
        fs::path cand3 = base.parent_path() / fs::u8path(p);
        if (fs::is_regular_file(cand3)) return cand3.u8string();
    }

    return "";
}

static std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string VirtualFileSystem::FindNoCase(const std::string& rel_path) {
    if (!nocase_built_) {
        nocase_built_ = true;
        std::error_code ec;
        const fs::path base = fs::u8path(asset_root_);
        for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) nocase_[Lower(fs::relative(it->path(), base, ec).generic_u8string())] = it->path().u8string();
    }
    auto it = nocase_.find(Lower(rel_path));
    if (it == nocase_.end()) return "";
    if (preloaded_) return it->second;   // (Delete keeps the index current)
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(it->second), ec) ? it->second : "";   // (deleted since)
}

// Path used when the game creates/overwrites a file (saves, settings).
// Creates any missing parent directories.
std::string VirtualFileSystem::WritablePath(const std::string& rel_path) {
    fs::path full = fs::u8path(asset_root_) / fs::u8path(NormalisePath(rel_path));
    std::error_code ec;
    if (full.has_parent_path()) fs::create_directories(full.parent_path(), ec);
    if (nocase_built_) nocase_[Lower(NormalisePath(rel_path))] = full.u8string();   // findable by any capitals later
    return full.u8string();
}

bool VirtualFileSystem::CheckExists(const std::string& path) {
    return !ResolvePath(path).empty();
}

uint32_t VirtualFileSystem::Open(const std::string& path, const std::string& mode) {
    const bool want_write  = mode.find('w') != std::string::npos;
    const bool want_append = mode.find('a') != std::string::npos;
    const bool want_update = mode.find('+') != std::string::npos;

    std::string real = ResolvePath(path);
    if (real.empty()) {
        if (!want_write && !want_append) {
            if (VerboseLog()) std::cout << "  [VFS] Open('" << path << "', '" << mode << "') -> NOT FOUND" << std::endl;
            return 0;
        }
        real = WritablePath(path);
    }

    std::ios_base::openmode om = std::ios::binary;
    if (want_write) {
        om |= std::ios::out | std::ios::trunc;
        if (want_update) om |= std::ios::in;
    } else if (want_append) {
        om |= std::ios::out | std::ios::app;
        if (want_update) om |= std::ios::in;
    } else {
        om |= std::ios::in;
        if (want_update) om |= std::ios::out;
    }

    uint32_t handle = next_handle_;
    next_handle_ += 0x100;

    auto cached = ram_.find(real);
    if (cached != ram_.end() && !(om & std::ios::out)) {   // preloaded: read from memory
        open_files_[handle] = std::make_unique<RamStream>(cached->second);
    } else if (preloaded_ && !(om & std::ios::out) && real.compare(0, asset_root_.size(), asset_root_) == 0) {
        // indexed only (PreloadAll(false)): the whole file in one read, held by the stream until Close
        if (auto recent = TakeRecent(real)) {
            open_files_[handle] = std::make_unique<RamStream>(std::move(recent));
        } else if (auto data = ReadWhole(real)) {
            AddRecent(real, data);
            open_files_[handle] = std::make_unique<RamStream>(std::move(data));
        }
    } else {
        if (cached != ram_.end()) ram_.erase(cached);   // written: from now on the file on disk
        if (om & std::ios::out) { DropRecent(real); RetirePack(real); }
        auto f = std::make_unique<std::fstream>(fs::u8path(real), om);
        if (f->is_open()) open_files_[handle] = std::move(f);
    }
    if (!open_files_.count(handle)) {
        std::cerr << "  [VFS] Failed to open real file: " << real << std::endl;
        return 0;
    }

    if (VerboseLog()) std::cout << "  [VFS] Open('" << path << "', '" << mode << "') -> handle 0x" << std::hex << handle
              << std::dec << " (" << real << ")" << std::endl;
    return handle;
}

uint32_t VirtualFileSystem::OpenFromMemory(uint32_t guest_addr, uint32_t size) {
    if (guest_addr == 0) {
        if (VerboseLog()) std::cout << "  [VFS] OpenFromMemory(NULL, " << size << ") -> 0" << std::endl;
        return 0;
    }
    uint32_t handle = next_handle_;
    next_handle_ += 0x100;
    mem_files_[handle] = MemFile{guest_addr, size, 0, false};
    if (VerboseLog()) std::cout << "  [VFS] OpenFromMemory(0x" << std::hex << guest_addr << ", 0x" << size
              << ") -> handle 0x" << handle << std::dec << std::endl;
    return handle;
}

void VirtualFileSystem::Close(uint32_t handle) {
    auto it = open_files_.find(handle);
    if (it != open_files_.end()) {
        open_files_.erase(it);   // (closes a disk file)
        disk_eof_.erase(handle);
        return;
    }
    mem_files_.erase(handle);
}

uint32_t VirtualFileSystem::Read(uint32_t handle, void* buffer, uint32_t elem_size, uint32_t count) {
    if (elem_size == 0 || count == 0) return 0;
    const uint64_t want = static_cast<uint64_t>(elem_size) * count;

    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) {
        MemFile& m = mit->second;
        uint64_t avail = (m.pos < m.size) ? (m.size - m.pos) : 0;
        uint32_t elems = static_cast<uint32_t>((std::min)(want, avail) / elem_size);
        uint32_t bytes = elems * elem_size;
        if (bytes && uc_) uc_->MemRead(m.addr + m.pos, buffer, bytes);
        m.pos += bytes;
        if (elems < count) m.eof = true;
        return elems;
    }

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return 0;
    it->second->read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(want));
    std::streamsize bytes_read = it->second->gcount();
    if (!*it->second) { it->second->clear(); disk_eof_.insert(handle); }
    return static_cast<uint32_t>(bytes_read / elem_size);
}

uint32_t VirtualFileSystem::Write(uint32_t handle, const void* buffer, uint32_t elem_size, uint32_t count) {
    if (elem_size == 0 || count == 0) return 0;
    const uint64_t want = static_cast<uint64_t>(elem_size) * count;

    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) {
        MemFile& m = mit->second;
        uint64_t avail = (m.pos < m.size) ? (m.size - m.pos) : 0;
        uint32_t elems = static_cast<uint32_t>((std::min)(want, avail) / elem_size);
        uint32_t bytes = elems * elem_size;
        if (bytes && uc_) uc_->MemWrite(m.addr + m.pos, buffer, bytes);
        m.pos += bytes;
        if (elems < count) {
            std::cout << "  [VFS] Memory file 0x" << std::hex << handle << " full (size 0x" << m.size
                      << ")" << std::dec << std::endl;
        }
        return elems;
    }

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return 0;
    auto before = it->second->tellp();
    it->second->write(reinterpret_cast<const char*>(buffer), static_cast<std::streamsize>(want));
    if (!*it->second) {
        it->second->clear();
        auto after = it->second->tellp();
        if (before < 0 || after < before) return 0;
        return static_cast<uint32_t>((after - before) / elem_size);
    }
    return count;
}

int VirtualFileSystem::Seek(uint32_t handle, int32_t offset, int origin) {
    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) {
        MemFile& m = mit->second;
        int64_t basepos = (origin == 1) ? m.pos : (origin == 2) ? m.size : 0;
        int64_t np = basepos + offset;
        if (np < 0 || np > m.size) return 1; // S3E_RESULT_ERROR
        m.pos = static_cast<uint32_t>(np);
        m.eof = false;
        return 0;
    }

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return 1;
    }

    std::ios_base::seekdir dir = std::ios::beg;
    if (origin == 1) dir = std::ios::cur;
    else if (origin == 2) dir = std::ios::end;

    it->second->clear();
    it->second->seekg(offset, dir); // filebuf has one shared get/put position
    if (!*it->second) { it->second->clear(); return 1; } // invalid seek leaves the position unchanged
    disk_eof_.erase(handle);
    return 0;
}

int32_t VirtualFileSystem::Tell(uint32_t handle) {
    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) return static_cast<int32_t>(mit->second.pos);

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return -1;
    }
    return static_cast<int32_t>(it->second->tellg());
}

uint32_t VirtualFileSystem::GetSize(uint32_t handle) {
    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) return mit->second.size;

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return 0;
    }
    it->second->clear();
    auto cur = it->second->tellg();
    it->second->seekg(0, std::ios::end);
    auto sz = it->second->tellg();
    it->second->seekg(cur, std::ios::beg);
    return static_cast<uint32_t>(sz);
}

bool VirtualFileSystem::EOF_Reached(uint32_t handle) {
    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) return mit->second.eof || mit->second.pos >= mit->second.size;

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return true;
    }
    return disk_eof_.count(handle) != 0 || it->second->tellg() >= static_cast<std::streampos>(GetSize(handle));
}

int32_t VirtualFileSystem::GetChar(uint32_t handle) {
    auto mit = mem_files_.find(handle);
    if (mit != mem_files_.end()) {
        MemFile& m = mit->second;
        if (m.pos >= m.size || !uc_) { m.eof = true; return -1; }
        uint8_t c = 0;
        uc_->MemRead(m.addr + m.pos, &c, 1);
        m.pos++;
        return c;
    }

    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return -1;
    }
    char c;
    if (it->second->get(c)) {
        return static_cast<uint8_t>(c);
    }
    it->second->clear();
    disk_eof_.insert(handle);
    return -1;
}

int32_t VirtualFileSystem::PutChar(uint32_t handle, uint8_t c) {
    return Write(handle, &c, 1, 1) == 1 ? c : -1;
}

void VirtualFileSystem::Flush(uint32_t handle) {
    auto it = open_files_.find(handle);
    if (it != open_files_.end()) it->second->flush();
}

bool VirtualFileSystem::Delete(const std::string& path) {
    std::string real = ResolvePath(path);
    if (real.empty()) return false;
    std::error_code ec;
    bool ok = fs::remove(fs::u8path(real), ec);
    if (ok) {
        ram_.erase(real);
        DropRecent(real);
        RetirePack(real);
        for (auto n = nocase_.begin(); n != nocase_.end();) n = n->second == real ? nocase_.erase(n) : std::next(n);
    }
    if (VerboseLog()) std::cout << "  [VFS] Delete('" << path << "') -> " << (ok ? "ok" : "failed") << std::endl;
    return ok;
}

bool VirtualFileSystem::MakeDirectory(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(asset_root_) / NormalisePath(path), ec);
    return !ec;
}

std::shared_ptr<const std::string> VirtualFileSystem::TakeRecent(const std::string& real) {
    for (auto it = recent_.begin(); it != recent_.end(); ++it)
        if (it->first == real) {
            recent_.splice(recent_.begin(), recent_, it);
            return it->second;
        }
    return nullptr;
}

void VirtualFileSystem::AddRecent(const std::string& real, std::shared_ptr<const std::string> data) {
    if (data->size() > recent_budget_) return;
    recent_bytes_ += data->size();
    recent_.emplace_front(real, std::move(data));
    while (recent_bytes_ > recent_budget_) {
        recent_bytes_ -= recent_.back().second->size();
        recent_.pop_back();
    }
}

void VirtualFileSystem::DropRecent(const std::string& real) {
    for (auto it = recent_.begin(); it != recent_.end(); ++it)
        if (it->first == real) { recent_bytes_ -= it->second->size(); recent_.erase(it); return; }
}

std::shared_ptr<std::string> VirtualFileSystem::ReadWhole(const std::string& real) {
    auto data = std::make_shared<std::string>();
    if (auto e = pack_entries_.find(real); e != pack_entries_.end() && pack_) {
        data->resize(e->second.size);
        if (fseek(pack_, static_cast<long>(e->second.offset), SEEK_SET) == 0 &&
            fread(data->data(), 1, data->size(), pack_) == data->size()) return data;
        std::cerr << "[VFS] Pack read failed for " << real << ", reading the file" << std::endl;
    }
    FILE* fp = fopen(real.c_str(), "rb");
    if (!fp) return nullptr;
    data->clear();
    if (fseek(fp, 0, SEEK_END) == 0) {
        const long size = ftell(fp);
        if (size > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            data->resize(static_cast<size_t>(size));
            data->resize(fread(data->data(), 1, data->size(), fp));
        }
    }
    fclose(fp);
    return data;
}

// data.pak: "MGSPAK1\n", u32 index offset, u32 count, the files' bytes, then the index: per file u16 length,
// the path below the data folder (lower case, '/'), u32 offset, u32 size. Little-endian.
namespace {
constexpr char kPackMagic[8] = {'M', 'G', 'S', 'P', 'A', 'K', '1', '\n'};
}

bool VirtualFileSystem::OpenPack(const std::vector<std::pair<std::string, std::string>>& files) {
    pack_entries_.clear();
    if (pack_) { fclose(pack_); pack_ = nullptr; }
    FILE* f = fopen(pack_path_.c_str(), "rb");
    if (!f) return false;
    char magic[8];
    uint32_t head[2] = {};
    bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, kPackMagic, 8) == 0 && fread(head, 4, 2, f) == 2;
    std::vector<char> index;
    if (ok) {   // the index in one read
        ok = fseek(f, 0, SEEK_END) == 0;
        const long end = ftell(f);
        ok = ok && end >= static_cast<long>(head[0]) && fseek(f, static_cast<long>(head[0]), SEEK_SET) == 0;
        if (ok) {
            index.resize(static_cast<size_t>(end - static_cast<long>(head[0])));
            ok = fread(index.data(), 1, index.size(), f) == index.size();
        }
    }
    std::unordered_map<std::string, PackEntry> by_rel;
    size_t p = 0;
    for (uint32_t i = 0; ok && i < head[1]; ++i) {
        uint16_t len = 0;
        PackEntry e{};
        if (p + 2 > index.size()) { ok = false; break; }
        memcpy(&len, &index[p], 2); p += 2;
        if (p + len + 8 > index.size()) { ok = false; break; }
        std::string rel(&index[p], len); p += len;
        memcpy(&e.offset, &index[p], 4); memcpy(&e.size, &index[p + 4], 4); p += 8;
        by_rel[rel] = e;
    }
    size_t want = 0;
    for (const auto& [real, rel] : files) {
        if (!ok) break;
        if (rel.find('/') == std::string::npos) continue;   // (top-level files: never packed)
        ++want;
        auto it = by_rel.find(Lower(rel));
        if (it == by_rel.end()) { ok = false; break; }
        pack_entries_[real] = it->second;
    }
    if (!ok || want != by_rel.size()) {   // damaged, or the folder has other files than the pack
        pack_entries_.clear();
        fclose(f);
        return false;
    }
    setvbuf(f, nullptr, _IONBF, 0);   // (whole files per read: no stdio buffer in between)
    pack_ = f;
    return true;
}

bool VirtualFileSystem::BuildPack(const std::vector<std::pair<std::string, std::string>>& files) {
    const std::string tmp = pack_path_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    setvbuf(f, nullptr, _IOFBF, 64 * 1024);
    std::string index;
    uint32_t count = 0, offset = 16;
    bool ok = fwrite(kPackMagic, 1, 8, f) == 8 && fwrite(&offset, 4, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1;
    for (const auto& [real, rel] : files) {
        if (!ok) break;
        if (rel.find('/') == std::string::npos) continue;
        auto data = ReadWhole(real);
        if (!data) { ok = false; break; }
        ok = data->empty() || fwrite(data->data(), 1, data->size(), f) == data->size();
        const std::string key = Lower(rel);
        const uint16_t len = static_cast<uint16_t>(key.size());
        const uint32_t size = static_cast<uint32_t>(data->size());
        index.append(reinterpret_cast<const char*>(&len), 2);
        index += key;
        index.append(reinterpret_cast<const char*>(&offset), 4);
        index.append(reinterpret_cast<const char*>(&size), 4);
        offset += size;
        ++count;
    }
    ok = ok && fwrite(index.data(), 1, index.size(), f) == index.size() && fseek(f, 8, SEEK_SET) == 0 &&
         fwrite(&offset, 4, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) {
        fs::remove(fs::u8path(pack_path_), ec);
        fs::rename(fs::u8path(tmp), fs::u8path(pack_path_), ec);
        ok = !ec;
    }
    if (!ok) fs::remove(fs::u8path(tmp), ec);
    std::cout << "[VFS] " << (ok ? "Built " : "Could not build ") << pack_path_ << ": " << count << " files, " << offset / 1024 << " KB" << std::endl;
    return ok;
}

void VirtualFileSystem::RetirePack(const std::string& real) {
    if (!pack_ || !pack_entries_.count(real)) return;
    std::cout << "[VFS] " << real << " changed: the pack is rebuilt on the next start" << std::endl;
    fclose(pack_);
    pack_ = nullptr;
    pack_entries_.clear();
    std::error_code ec;
    fs::remove(fs::u8path(pack_path_), ec);
}

size_t VirtualFileSystem::PreloadAll(bool load_data, const std::string& pack_path, const std::function<void()>& on_build) {
    nocase_.clear();
    nocase_built_ = true;
    ram_.clear();
    size_t total = 0;
    std::error_code ec;
    const fs::path base = fs::u8path(asset_root_);
    const size_t root_len = base.generic_u8string().size();
    std::vector<std::pair<std::string, std::string>> files;   // (real, rel)
    for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string real = it->path().u8string();
        // (fs::relative returns nothing for "sdmc:/" paths on the 3DS: strip the root as text)
        std::string rel = it->path().generic_u8string().substr(std::min(root_len, it->path().generic_u8string().size()));
        while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        nocase_[Lower(rel)] = real;
        files.emplace_back(real, rel);
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    preloaded_ = true;
    if (!pack_path.empty()) {
        pack_path_ = pack_path;
        if (!OpenPack(files)) {
            if (on_build) on_build();
            if (BuildPack(files)) OpenPack(files);
        }
        std::cout << "[VFS] Pack: " << (pack_ ? std::to_string(pack_entries_.size()) + " files" : std::string("none (files read one by one)")) << std::endl;
    }
    if (load_data)
        for (const auto& [real, rel] : files)
            if (auto data = ReadWhole(real)) { total += data->size(); ram_[real] = std::move(data); }
    if (VerboseLog()) for (const auto& [k, v] : nocase_) { std::cout << "[VFS] Index example: '" << k << "' -> " << v << std::endl; break; }
    if (load_data) std::cout << "[VFS] Preloaded " << ram_.size() << " files, " << total / 1024 << " KB" << std::endl;
    else std::cout << "[VFS] Indexed " << nocase_.size() << " files" << std::endl;
    return total;
}
