#include "textures.h"
#include "pngio.h"
#include "miniz.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace {
// IwGx CIwImage formats (the game's table at 0x4A0F02E0, 6 bytes each): u16 = palette size code (bits 0-4:
// 1/2/4/8/16 -> 16/32/64/128/256 entries), alpha (bit 5), reversed component order (bit 6: ABGR instead of
// RGBA), bytes per pixel or palette entry (bits 7-10); then the R, G, B, A bit widths.
struct ImageFormat { uint16_t code; uint8_t bits[4]; };
const ImageFormat kFormats[0x26] = {
    {0x0000, {0, 0, 0, 0}}, {0x0080, {3, 3, 2, 0}}, {0x00C0, {3, 3, 2, 0}}, {0x0100, {5, 6, 5, 0}},
    {0x0140, {5, 6, 5, 0}}, {0x0120, {4, 4, 4, 4}}, {0x0160, {4, 4, 4, 4}}, {0x0120, {5, 5, 5, 1}},
    {0x0160, {5, 5, 5, 1}}, {0x0180, {8, 8, 8, 0}}, {0x01C0, {8, 8, 8, 0}}, {0x01A0, {6, 6, 6, 6}},
    {0x01E0, {6, 6, 6, 6}}, {0x0220, {8, 8, 8, 8}}, {0x0260, {8, 8, 8, 8}}, {0x0220, {10, 10, 10, 2}},
    {0x0260, {10, 10, 10, 2}}, {0x0181, {8, 8, 8, 0}}, {0x0221, {8, 8, 8, 8}}, {0x0101, {5, 6, 5, 0}},
    {0x0121, {4, 4, 4, 4}}, {0x0121, {5, 5, 5, 1}}, {0x0161, {5, 5, 5, 1}}, {0x0190, {8, 8, 8, 0}},
    {0x0230, {8, 8, 8, 8}}, {0x0110, {5, 6, 5, 0}}, {0x0130, {4, 4, 4, 4}}, {0x0130, {5, 5, 5, 1}},
    {0x0170, {5, 5, 5, 1}}, {0x0168, {5, 5, 5, 1}}, {0x0164, {5, 5, 5, 1}}, {0x0162, {5, 5, 5, 1}},
    {0x0100, {4, 4, 4, 4}}, {0x0100, {4, 4, 4, 4}}, {0x0100, {4, 4, 4, 4}}, {0x0100, {4, 4, 4, 4}},
    {0x0161, {4, 4, 4, 4}}, {0x0170, {4, 4, 4, 4}},
};
constexpr uint32_t kCIwTextureHash = 0x3521F539;   // lower-case djb2 of "CIwTexture" (0x4A01F238)

int PaletteEntries(const ImageFormat& f) {
    switch (f.code & 0x1F) { case 1: return 16; case 2: return 32; case 4: return 64; case 8: return 128; case 16: return 256; default: return 0; }
}
int BytesPer(const ImageFormat& f) { return (f.code >> 7) & 15; }

struct Texture {
    uint32_t hash = 0, ref = 0;
    uint8_t fmt = 0;
    uint16_t w = 0, h = 0, pitch = 0;
    std::vector<uint8_t> texels, palette;
    std::string group;   // path of the group file under the asset root, without ".group.bin"
};

struct Reader {
    const std::vector<uint8_t>& d;
    size_t o;
    bool ok = true;
    bool Need(size_t n) { if (o + n > d.size()) ok = false; return ok; }
    uint8_t U8() { if (!Need(1)) return 0; return d[o++]; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = d[o] | d[o + 1] << 8; o += 2; return v; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v = d[o] | d[o + 1] << 8 | d[o + 2] << 16 | static_cast<uint32_t>(d[o + 3]) << 24; o += 4; return v; }
    std::vector<uint8_t> Bytes(size_t n) { if (!Need(n)) return {}; std::vector<uint8_t> v(d.begin() + o, d.begin() + o + n); o += n; return v; }
};

// CIwTexture::Serialise (0x4A06103C): CIwManaged hash (0x4A05EEF8), u32 flags (+0x34), u8 +0x40, u8 +0x41,
// 2 x u16 (+0xC0, resource version >= 0x30501). Flag 0x1000: a palette-only texture that shares the texels of the
// texture whose hash follows (u32 +0xB4, image header without texels, 0x4A0B3698). Otherwise CIwImage::Serialise
// (0x4A0B5EF4): u8 format, u16 flags, u16 width, height, pitch, u32 compressed size (version > 0x30400), the
// texels (pitch x height, or that size for formats 0x20..0x23), then the palette (entries x bytes) if paletted.
bool ParseTexture(Reader& r, uint32_t ver, Texture& t) {
    t.hash = r.U32();
    const uint32_t flags = r.U32();
    r.U8(); r.U8();
    if (ver >= 0x30501) { r.U16(); r.U16(); }
    if (flags & 0x1000) t.ref = r.U32();
    t.fmt = r.U8();
    r.U16();
    t.w = r.U16(); t.h = r.U16(); t.pitch = r.U16();
    if (!r.ok || t.fmt >= 0x26) return false;
    const ImageFormat& f = kFormats[t.fmt];
    if (!(flags & 0x1000)) {
        uint32_t size = ver > 0x30400 ? r.U32() : 0;
        if (t.fmt < 0x20 || t.fmt > 0x23) size = static_cast<uint32_t>(t.pitch) * t.h;
        t.texels = r.Bytes(size);
    }
    if (const int n = PaletteEntries(f)) t.palette = r.Bytes(static_cast<size_t>(n) * BytesPer(f));
    return r.ok;
}

// A resource list of textures: class hash, u32 count, u8, then per texture a u8 and the texture.
void ParseGroup(const std::vector<uint8_t>& d, const std::string& group, std::vector<Texture>& out) {
    if (d.size() < 8) return;
    const uint32_t ver = d[1] << 16 | d[2] << 8 | d[3];   // header "=" + version 3.5.5 / 3.4.8
    for (size_t i = 0; i + 9 <= d.size(); ++i) {
        if (memcmp(&d[i], &kCIwTextureHash, 4) != 0) continue;
        Reader r{d, i + 4};
        const uint32_t n = r.U32();
        if (n == 0 || n > 4096) continue;
        r.U8();
        std::vector<Texture> list;
        for (uint32_t k = 0; k < n && r.ok; ++k) {
            r.U8();
            Texture t;
            t.group = group;
            if (!ParseTexture(r, ver, t)) break;
            list.push_back(std::move(t));
        }
        if (list.size() != n) continue;   // a false match on the class hash
        for (auto& t : list) out.push_back(std::move(t));
        i = r.o - 1;
    }
}

uint8_t Expand(uint32_t v, int bits) { return bits ? static_cast<uint8_t>(v * 255 / ((1u << bits) - 1)) : 255; }

// One pixel or palette entry -> 0xAARRGGBB. Packed little-endian, R in the top bits (A in the low bits) for
// RGBA formats and the other way round for the reversed ones (ABGR1555: A = bit 15, R = bits 0-4); checked
// against the 16-bit textures IwGx uploads.
uint32_t Colour(const uint8_t* e, const ImageFormat& f) {
    const int nb = BytesPer(f);
    uint32_t v = 0;
    for (int i = 0; i < nb; ++i) v |= static_cast<uint32_t>(e[i]) << (8 * i);
    const int R = f.bits[0], G = f.bits[1], B = f.bits[2], A = f.bits[3];
    int sr, sg, sb, sa;
    if (f.code & 0x40) { sr = 0; sg = R; sb = R + G; sa = R + G + B; }
    else { sa = 0; sb = A; sg = A + B; sr = A + B + G; }
    auto take = [&](int shift, int bits) { return Expand((v >> shift) & ((1u << bits) - 1), bits); };
    const uint8_t r = take(sr, R), g = take(sg, G), b = take(sb, B), a = A ? take(sa, A) : 255;
    return static_cast<uint32_t>(a) << 24 | r << 16 | g << 8 | b;
}

bool Decode(const Texture& t, const std::vector<uint8_t>& texels, std::vector<uint32_t>& out) {
    const ImageFormat& f = kFormats[t.fmt];
    const int entries = PaletteEntries(f), nb = BytesPer(f);
    if (!t.w || !t.h || !nb || (t.fmt >= 0x20 && t.fmt <= 0x23)) return false;   // compressed: not used here
    out.assign(static_cast<size_t>(t.w) * t.h, 0);
    std::vector<uint32_t> pal(256, 0xFFFF00FF);
    for (int i = 0; i < entries && static_cast<size_t>(i + 1) * nb <= t.palette.size(); ++i) pal[i] = Colour(&t.palette[i * nb], f);
    for (int y = 0; y < t.h; ++y) {
        const size_t row = static_cast<size_t>(y) * t.pitch;
        for (int x = 0; x < t.w; ++x) {
            uint32_t c = 0;
            if (entries == 16) {   // 4 bpp, low nibble first
                const size_t b = row + x / 2;
                if (b < texels.size()) c = pal[(x & 1) ? texels[b] >> 4 : texels[b] & 15];
            } else if (entries) {
                const size_t b = row + x;
                if (b < texels.size()) c = pal[texels[b]];
            } else {
                const size_t b = row + static_cast<size_t>(x) * nb;
                if (b + nb <= texels.size()) c = Colour(&texels[b], f);
            }
            out[static_cast<size_t>(y) * t.w + x] = c;
        }
    }
    return true;
}

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t FileCrc(const fs::path& p, uint64_t* size) {
    std::vector<uint8_t> d;
    if (!ReadFile(p, d)) return 0;
    if (size) *size = d.size();
    return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, d.data(), d.size()));
}

const char* kReadme =
    "Metal Gear Solid Mobile - textures\r\n"
    "==================================\r\n"
    "\r\n"
    "Every texture in the game, extracted from your copy as PNG. The folders follow the game's resource\r\n"
    "groups (common\\ui\\hud = the HUD, chapters\\facility\\mlist_N = level materials, ...). Each file is named\r\n"
    "after the texture's ID, which is how the game finds it.\r\n"
    "\r\n"
    "Custom textures: edit a PNG in place, or put a PNG with the same name anywhere in this folder. It can be any\r\n"
    "size (higher resolution is fine) and may use transparency. Changed files are picked up when the game starts.\r\n"
    "Unchanged files are ignored (the game uses its own copy), so you only need to keep the ones you edit.\r\n"
    "Custom textures can be switched off in the F1 menu.\r\n"
    "\r\n"
    "originals.txt lists the extracted files; delete this whole folder to extract them again on the next start.\r\n";

std::mutex g_mutex;
std::map<uint32_t, fs::path> g_replacements;
std::map<uint32_t, std::string> g_original_paths;   // hash -> path in originals.txt (relative, as written)
std::atomic<bool> g_enabled{true};

bool ParseHashName(const fs::path& p, uint32_t& hash) {
    const std::wstring stem = p.stem().wstring();
    if (stem.size() != 8 || p.extension() != L".png") return false;
    wchar_t* end = nullptr;
    hash = static_cast<uint32_t>(wcstoul(stem.c_str(), &end, 16));
    return end && *end == 0;
}
}

int TexturePack::Extract(const std::string& asset_root, const fs::path& out_dir,
                         const std::function<void(int, int)>& progress, std::string* error) {
    std::vector<Texture> all;
    std::error_code ec;
    const fs::path root = fs::u8path(asset_root);
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string suffix = ".group.bin";
        if (!it->is_regular_file() || name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        std::vector<uint8_t> d;
        if (!ReadFile(it->path(), d)) continue;
        std::string rel = fs::relative(it->path(), root).string();
        rel.resize(rel.size() - suffix.size());
        ParseGroup(d, rel, all);
    }
    if (all.empty()) { if (error) *error = "no textures found under " + asset_root; return -1; }

    std::map<uint32_t, const Texture*> by_hash;   // a texture shared by several groups is written once (first group)
    for (const Texture& t : all) by_hash.emplace(t.hash, &t);
    const fs::path out = out_dir;
    fs::create_directories(out, ec);
    std::ostringstream manifest;
    manifest << "# Textures as extracted: <file> <size> <crc32>. A PNG that still matches is the original (not loaded).\n";
    int written = 0, done = 0;
    const int total = static_cast<int>(by_hash.size());
    for (const auto& [hash, tp] : by_hash) {
        const Texture& t = *tp;
        const std::vector<uint8_t>* texels = &t.texels;
        if (t.ref) {
            auto r = by_hash.find(t.ref);
            if (r == by_hash.end()) { ++done; continue; }
            texels = &r->second->texels;
        }
        std::vector<uint32_t> px;
        if (Decode(t, *texels, px)) {
            const fs::path dir = out / fs::u8path(t.group);
            fs::create_directories(dir, ec);
            char name[16];
            snprintf(name, sizeof name, "%08x.png", hash);
            const fs::path file = dir / name;
            const std::string err = PngIO::Write(file, px.data(), t.w, t.h);
            if (!err.empty()) { if (error) *error = "writing " + file.string() + ": " + err; return -1; }
            uint64_t size = 0;
            const uint32_t crc = FileCrc(file, &size);
            manifest << fs::relative(file, out).string() << '\t' << size << '\t' << std::hex << crc << std::dec << '\n';
            ++written;
        }
        if (progress) progress(++done, total);
    }
    std::ofstream(out / "README.txt", std::ios::binary) << kReadme;
    std::ofstream(out / "originals.txt", std::ios::binary) << manifest.str();   // last: marks a complete extraction
    std::cout << "[Textures] Extracted " << written << " textures to " << out.string() << std::endl;
    return written;
}

bool TexturePack::Extracted(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "originals.txt", ec);
}

void TexturePack::Init(const fs::path& dir) {
    std::map<std::string, std::pair<uint64_t, uint32_t>> originals;   // lower-case relative path -> size, crc
    std::map<uint32_t, std::string> paths;
    const fs::path root = dir;
    {
        std::ifstream m(root / "originals.txt");
        std::string line;
        while (std::getline(m, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string file; uint64_t size = 0; uint32_t crc = 0;
            if (std::getline(ls, file, '\t') && ls >> size >> std::hex >> crc) {
                uint32_t hash = 0;
                if (ParseHashName(fs::path(file), hash)) paths[hash] = file;
                for (char& c : file) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                originals[file] = {size, crc};
            }
        }
    }
    std::map<uint32_t, fs::path> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        uint32_t hash = 0;
        if (!it->is_regular_file() || !ParseHashName(it->path(), hash)) continue;
        std::string rel = fs::relative(it->path(), root).string();
        for (char& c : rel) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        auto o = originals.find(rel);
        if (o != originals.end() && it->file_size() == o->second.first) {
            uint64_t size = 0;
            if (FileCrc(it->path(), &size) == o->second.second) continue;   // unchanged original
        }
        // Edited in place wins over a copy elsewhere with the same name.
        if (!found.count(hash) || o != originals.end()) found[hash] = it->path();
    }
    std::lock_guard<std::mutex> l(g_mutex);
    g_replacements = std::move(found);
    g_original_paths = std::move(paths);
    std::cout << "[Textures] " << g_replacements.size() << " custom texture(s) in " << root.string() << std::endl;
}

int TexturePack::ReplacementCount() {
    std::lock_guard<std::mutex> l(g_mutex);
    return static_cast<int>(g_replacements.size());
}

void TexturePack::SetEnabled(bool on) { g_enabled = on; }
bool TexturePack::Enabled() { return g_enabled; }

bool TexturePack::Replacement(uint32_t hash, std::vector<uint32_t>& rgba, int& w, int& h) {
    if (!g_enabled) return false;
    fs::path p;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_replacements.find(hash);
        if (it == g_replacements.end()) return false;
        p = it->second;
    }
    if (PngIO::ReadRGBA(p, rgba, w, h)) return true;
    std::cout << "[Textures] Could not read " << p.string() << std::endl;
    return false;
}

std::string TexturePack::OriginalPath(uint32_t hash) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_original_paths.find(hash);
    return it == g_original_paths.end() ? std::string() : it->second;
}

bool TexturePack::HasReplacement(uint32_t hash) {
    std::lock_guard<std::mutex> l(g_mutex);
    return g_replacements.count(hash) != 0;
}
