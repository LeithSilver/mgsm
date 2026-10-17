#include "installer.h"
#include "loader.h"
#include "textures.h"

#include "miniz.h"
extern "C" {
#include "LzmaDec.h"
}

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {
// ---- the supported package ------------------------------------------------------------------------------
// Metal.Gear.Solid.n-gage (2,542,447 bytes): a MIME envelope whose <Game> part is a Symbian 9 SIS. The files
// we need, by install path and CRC-32:
constexpr uint32_t kCrcAllDz = 0x2FCD5F69;       // \private\20010925\all.dz       (1,286,000 B; 266 files)
constexpr uint32_t kCrcWarpDz = 0x7286FDF5;      // \private\20010925\warp.dz      (3,903 B)
constexpr uint32_t kCrcS3e = 0xB3A17DF6;         // \private\20010925\mgs_ngi.s3e  (LZMA)
constexpr uint32_t kCrcBinary = 0x3F53F5D1;      //   -> mgs_ngi (XE3U, 1,091,029 B)
constexpr uint32_t kCrcScrambled = 0xD37FE360;   // \private\20010925\bin\encrypted.dz.ngdat (88,212 B)
constexpr uint32_t kCrcScripts = 0x74684A11;     //   -> the plain archive of the 38 full-version scripts

// encrypted.dz.ngdat carries a sparse scramble layer that the N-Gage launcher removed at runtime (README.md
// §6.11): 413 bytes in 147 runs. This XOR table (offset, length, bytes) restores the plain archive.
struct XorRun { uint32_t offset; uint8_t len; uint8_t x[8]; };
const XorRun kScriptsXor[] = {
    {0x000AA, 4, {0xFE, 0x78, 0xB0, 0x98}},
    {0x0018C, 3, {0x88, 0x5E, 0x7C}},
    {0x00190, 1, {0x20}},
    {0x00255, 2, {0x92, 0x82}},
    {0x003D3, 1, {0xC0}},
    {0x003D5, 3, {0x26, 0x14, 0xFC}},
    {0x0068E, 4, {0xFE, 0xFC, 0xFE, 0xFC}},
    {0x00699, 1, {0xFE}},
    {0x0069B, 2, {0xFE, 0xFE}},
    {0x006C1, 1, {0xF8}},
    {0x006C3, 1, {0x02}},
    {0x0088F, 2, {0xFC, 0xFC}},
    {0x00A7D, 5, {0xEA, 0x68, 0xB4, 0x1C, 0x20}},
    {0x00CE1, 4, {0x3C, 0xCA, 0xD8, 0x2E}},
    {0x00E13, 3, {0x58, 0x7C, 0x7C}},
    {0x00F80, 5, {0xE0, 0x1C, 0xC2, 0xC0, 0xE2}},
    {0x01155, 5, {0x28, 0x32, 0x74, 0x36, 0x7E}},
    {0x01264, 4, {0x3E, 0xC0, 0xE0, 0xFC}},
    {0x0137D, 5, {0x08, 0x72, 0xFC, 0x06, 0xF0}},
    {0x014B5, 3, {0xE0, 0x20, 0xFC}},
    {0x01584, 2, {0xFC, 0xF4}},
    {0x01588, 1, {0x80}},
    {0x01866, 2, {0x70, 0xF8}},
    {0x01ADB, 5, {0xFC, 0x56, 0xFC, 0x72, 0x70}},
    {0x0200A, 1, {0x80}},
    {0x0200C, 2, {0x82, 0x0C}},
    {0x02096, 3, {0xC0, 0x38, 0x72}},
    {0x021EB, 1, {0x62}},
    {0x021ED, 2, {0x38, 0x60}},
    {0x022E3, 5, {0x20, 0xC0, 0xE8, 0x1C, 0xFA}},
    {0x0256F, 5, {0xFC, 0x80, 0x06, 0x70, 0xCA}},
    {0x027F8, 2, {0xF0, 0x90}},
    {0x02842, 1, {0xF8}},
    {0x02845, 1, {0xC0}},
    {0x028BF, 1, {0x80}},
    {0x028C1, 2, {0x78, 0x3C}},
    {0x029C4, 4, {0x7C, 0xD0, 0xF8, 0x02}},
    {0x02A3A, 5, {0x7C, 0x04, 0x7E, 0xC2, 0xFE}},
    {0x02A4C, 3, {0xFA, 0x64, 0x7E}},
    {0x02BD4, 5, {0x78, 0xC0, 0x1C, 0x0E, 0x1C}},
    {0x02C3A, 6, {0xFA, 0x3C, 0x3E, 0xF0, 0x46, 0x7E}},
    {0x02C41, 1, {0xEC}},
    {0x02DEB, 3, {0xF8, 0xE0, 0xB8}},
    {0x02EF5, 3, {0x1E, 0xFE, 0x06}},
    {0x03515, 1, {0xFA}},
    {0x03583, 4, {0xC0, 0xFC, 0x02, 0xFE}},
    {0x03589, 3, {0x08, 0xDE, 0x80}},
    {0x035B1, 3, {0x3C, 0x88, 0x80}},
    {0x036E3, 3, {0xF8, 0xF8, 0x7C}},
    {0x0375F, 1, {0xFC}},
    {0x03761, 2, {0xE0, 0x8A}},
    {0x03863, 3, {0x7C, 0xA8, 0xC0}},
    {0x0389C, 4, {0xE0, 0xFC, 0x40, 0x08}},
    {0x03C7F, 4, {0x88, 0x10, 0xF0, 0x80}},
    {0x03CAD, 2, {0x06, 0xFC}},
    {0x03DDC, 5, {0x04, 0xE0, 0x64, 0x8C, 0x10}},
    {0x03FE1, 3, {0x1C, 0x4C, 0x38}},
    {0x04137, 3, {0x60, 0x08, 0xFE}},
    {0x041ED, 4, {0x08, 0xC0, 0x80, 0x92}},
    {0x0429C, 5, {0xCE, 0x60, 0xE8, 0xF4, 0x5E}},
    {0x045D7, 3, {0xF0, 0xC0, 0x30}},
    {0x045DB, 1, {0x52}},
    {0x045E4, 4, {0x08, 0xB2, 0x40, 0xE0}},
    {0x0472E, 2, {0x60, 0x78}},
    {0x04C59, 5, {0x88, 0x10, 0xA0, 0x7C, 0x7E}},
    {0x05069, 1, {0xE0}},
    {0x0506B, 2, {0xC8, 0x0C}},
    {0x0521C, 4, {0x3C, 0x70, 0x9E, 0xF0}},
    {0x053EF, 2, {0x38, 0x32}},
    {0x056AF, 3, {0x20, 0x38, 0x0C}},
    {0x05971, 3, {0x70, 0xA6, 0x8C}},
    {0x05999, 4, {0x48, 0x06, 0x80, 0xE0}},
    {0x059C2, 1, {0x30}},
    {0x05BA1, 3, {0xF0, 0x04, 0x10}},
    {0x05D8A, 3, {0xFE, 0x88, 0xC0}},
    {0x05D9C, 2, {0x08, 0xC0}},
    {0x05DC6, 2, {0x38, 0x30}},
    {0x05DC9, 1, {0xE0}},
    {0x06051, 5, {0xA8, 0xF0, 0xC6, 0x9E, 0xE0}},
    {0x06247, 3, {0xC2, 0x18, 0xFE}},
    {0x06258, 3, {0x86, 0xC0, 0xFC}},
    {0x065DB, 2, {0x9C, 0xFC}},
    {0x065DE, 1, {0x0C}},
    {0x06812, 1, {0x38}},
    {0x06814, 1, {0xFE}},
    {0x06917, 1, {0x8C}},
    {0x06A6B, 1, {0xDE}},
    {0x06A6D, 2, {0x10, 0x46}},
    {0x06BC7, 1, {0xEE}},
    {0x06D31, 2, {0xFC, 0x04}},
    {0x06EFD, 4, {0xF2, 0x80, 0xB4, 0xC0}},
    {0x071A6, 4, {0xFC, 0xC0, 0xF0, 0x0E}},
    {0x071DC, 3, {0x80, 0x1C, 0x40}},
    {0x072C8, 3, {0xA0, 0xF8, 0x20}},
    {0x07389, 5, {0xC2, 0x80, 0x04, 0x72, 0xE0}},
    {0x073B0, 5, {0xC0, 0xFE, 0x60, 0x3C, 0xFC}},
    {0x07403, 3, {0xD0, 0x0C, 0x38}},
    {0x07889, 5, {0x30, 0x10, 0xE8, 0x20, 0x84}},
    {0x07BB6, 7, {0xF8, 0x9E, 0x70, 0x5C, 0xF0, 0x44, 0xF0}},
    {0x07C47, 4, {0x06, 0x94, 0x7E, 0xFC}},
    {0x07C9F, 3, {0x60, 0x3A, 0xF8}},
    {0x082C2, 2, {0xBE, 0xF0}},
    {0x08B0E, 2, {0x06, 0xC0}},
    {0x08B12, 1, {0xDE}},
    {0x0937A, 2, {0xCE, 0xC0}},
    {0x0A228, 4, {0xFE, 0x0C, 0x0C, 0x06}},
    {0x0B10E, 3, {0xD8, 0xF0, 0xFC}},
    {0x0B33B, 2, {0x38, 0xE0}},
    {0x0B3C8, 4, {0xF8, 0x80, 0xDC, 0x38}},
    {0x0BA12, 4, {0xE0, 0xFA, 0xFC, 0xE0}},
    {0x0BCB8, 4, {0xF0, 0x20, 0xF8, 0x80}},
    {0x0BF0F, 5, {0x60, 0x7E, 0x40, 0xBC, 0x38}},
    {0x0CAC5, 3, {0x24, 0x82, 0xF6}},
    {0x0CAF8, 4, {0xCE, 0x36, 0x20, 0x80}},
    {0x0D483, 5, {0x1C, 0xFE, 0xE0, 0x84, 0xEE}},
    {0x0D5E5, 1, {0xFC}},
    {0x0D5E7, 2, {0xE0, 0x0C}},
    {0x0E7E9, 4, {0xF0, 0xC6, 0x1E, 0x7C}},
    {0x0EF3B, 2, {0x78, 0xFC}},
    {0x0F226, 3, {0xF8, 0xBC, 0x3E}},
    {0x0FA9B, 3, {0x90, 0xF4, 0xDE}},
    {0x101C6, 1, {0xCE}},
    {0x101C8, 1, {0x32}},
    {0x110BA, 4, {0xF4, 0xE0, 0x02, 0xC8}},
    {0x11125, 3, {0x3E, 0xE0, 0x04}},
    {0x122BA, 2, {0x80, 0x9E}},
    {0x12896, 4, {0x3E, 0xDC, 0xE4, 0xFC}},
    {0x12A8B, 5, {0x18, 0x0E, 0x3C, 0x80, 0x1A}},
    {0x12C45, 2, {0xC0, 0xD8}},
    {0x12C48, 2, {0x30, 0x30}},
    {0x1383C, 4, {0x0C, 0xF0, 0x38, 0xCE}},
    {0x138EE, 1, {0xFC}},
    {0x138F0, 1, {0xC0}},
    {0x13B86, 3, {0x0E, 0x10, 0x80}},
    {0x13DCE, 1, {0x70}},
    {0x13DD0, 2, {0x10, 0x7E}},
    {0x140B6, 3, {0xEC, 0xE2, 0x84}},
    {0x14369, 3, {0x30, 0xFE, 0xC0}},
    {0x14691, 2, {0xFC, 0x8C}},
    {0x146C0, 2, {0xEC, 0x18}},
    {0x146C3, 1, {0x18}},
    {0x14EBB, 1, {0x10}},
    {0x14EBD, 1, {0x06}},
    {0x14F91, 3, {0xC2, 0x60, 0x80}},
    {0x14F95, 1, {0x80}},
    {0x15171, 5, {0xC6, 0x54, 0x78, 0xF8, 0xFC}},
    {0x1555D, 5, {0x7E, 0x20, 0xFA, 0x04, 0x88}},
};

uint32_t Crc(const Bytes& b) { return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, b.data(), b.size())); }

std::string Hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%08X", v); return b; }

bool ReadAll(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteAll(const fs::path& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

// ---- SIS (Symbian 9 software installation) --------------------------------------------------------------
// Fields: u32 type, u32 length (bit 31 set: 63-bit length, the next u32 holds the high bits), data padded to
// 4 bytes. Arrays: u32 element type, then elements without their type. Compressed: u32 algorithm (1 = zlib),
// u64 uncompressed size, data.
struct Field { uint32_t type = 0; const uint8_t* p = nullptr; size_t len = 0; };

bool NextField(const uint8_t* b, size_t n, size_t& o, Field& f, bool typed = true) {
    if (typed) { if (o + 4 > n) return false; memcpy(&f.type, b + o, 4); o += 4; }
    if (o + 4 > n) return false;
    uint32_t l; memcpy(&l, b + o, 4); o += 4;
    uint64_t len = l;
    if (l & 0x80000000u) { if (o + 4 > n) return false; uint32_t hi; memcpy(&hi, b + o, 4); o += 4; len = (l & 0x7FFFFFFFu) | (static_cast<uint64_t>(hi) << 31); }
    if (len > n - o) return false;
    f.p = b + o; f.len = static_cast<size_t>(len);
    o += f.len + ((4 - f.len % 4) % 4);
    return true;
}

std::vector<Field> Fields(const uint8_t* b, size_t n) {
    std::vector<Field> out; size_t o = 0; Field f;
    while (o + 8 <= n && NextField(b, n, o, f)) out.push_back(f);
    return out;
}

std::vector<Field> Array(const Field& a) {
    std::vector<Field> out;
    if (a.len < 4) return out;
    uint32_t et; memcpy(&et, a.p, 4);
    size_t o = 4; Field f;
    while (o < a.len && NextField(a.p, a.len, o, f, false)) { f.type = et; out.push_back(f); }
    return out;
}

bool Uncompress(const Field& c, Bytes& out) {
    if (c.len < 12) return false;
    uint32_t alg; uint64_t size;
    memcpy(&alg, c.p, 4); memcpy(&size, c.p + 4, 8);
    if (size > 256u * 1024 * 1024) return false;
    if (alg == 0) { out.assign(c.p + 12, c.p + c.len); return true; }
    out.resize(static_cast<size_t>(size));
    mz_ulong dl = static_cast<mz_ulong>(size);
    return mz_uncompress(out.data(), &dl, c.p + 12, static_cast<mz_ulong>(c.len - 12)) == MZ_OK && dl == size;
}

void CollectFiles(const uint8_t* b, size_t n, std::vector<Field>& descs) {
    for (const Field& f : Fields(b, n)) {
        if (f.type == 24) descs.push_back(f);                                  // SISFileDescription
        else if (f.type == 13 || f.type == 26 || f.type == 27 || f.type == 28) CollectFiles(f.p, f.len, descs);   // controller, if, else-if, install block
        else if (f.type == 2) {
            for (const Field& e : Array(f)) {
                if (e.type == 24) descs.push_back(e);
                else if (e.type == 26 || e.type == 27 || e.type == 28) CollectFiles(e.p, e.len, descs);
            }
        }
    }
}

// Every file of the package by lower-cased install path ("!:\private\20010925\all.dz").
bool ReadPackage(const Bytes& pkg, std::map<std::string, Bytes>& files, std::string* error) {
    size_t start = 0;
    const char* tag = "Content-ID: <Game>";
    auto it = std::search(pkg.begin(), pkg.end(), tag, tag + strlen(tag));
    if (it != pkg.end()) {   // .n-gage: the SIS is the <Game> MIME part
        const char* sep = "\r\n\r\n";
        auto body = std::search(it, pkg.end(), sep, sep + 4);
        if (body == pkg.end()) { *error = "The package's game part is damaged."; return false; }
        start = static_cast<size_t>(body - pkg.begin()) + 4;
    }
    const uint8_t* sis = pkg.data() + start;
    const size_t n = pkg.size() - start;
    uint32_t uid1 = 0;
    if (n > 16) memcpy(&uid1, sis, 4);
    if (uid1 != 0x10201A7A) { *error = "This isn't an N-Gage game package (.n-gage)."; return false; }
    size_t o = 16; Field contents;
    if (!NextField(sis, n, o, contents) || contents.type != 12) { *error = "The package is damaged (no SIS contents)."; return false; }
    Bytes controller; const Field* data = nullptr;
    std::vector<Field> parts = Fields(contents.p, contents.len);
    for (const Field& f : parts) {
        if (f.type == 3 && controller.empty() && !Uncompress(f, controller)) { *error = "The package is damaged (controller)."; return false; }
        if (f.type == 30) data = &f;
    }
    if (controller.empty() || !data) { *error = "The package is damaged (no file data)."; return false; }
    std::vector<Field> descs;
    CollectFiles(controller.data(), controller.size(), descs);
    std::vector<Field> file_data;   // SISData -> SISDataUnit[] -> SISFileData[]
    std::vector<Field> dparts = Fields(data->p, data->len);
    if (!dparts.empty()) {
        for (const Field& unit : Array(dparts[0]))
            for (const Field& f : Fields(unit.p, unit.len))
                if (f.type == 2) for (const Field& fd : Array(f)) file_data.push_back(fd);
    }
    for (const Field& d : descs) {
        std::vector<Field> fs_ = Fields(d.p, d.len);
        if (fs_.empty() || fs_[0].type != 1 || d.len < 28) continue;
        std::string target;
        for (size_t i = 0; i + 1 < fs_[0].len; i += 2) {
            const uint16_t c = fs_[0].p[i] | fs_[0].p[i + 1] << 8;
            target += static_cast<char>(c < 128 ? tolower(c) : '?');
        }
        uint32_t index; memcpy(&index, d.p + d.len - 4, 4);
        if (index >= file_data.size()) continue;
        std::vector<Field> c = Fields(file_data[index].p, file_data[index].len);
        Bytes raw;
        if (c.empty() || c[0].type != 3 || !Uncompress(c[0], raw)) { *error = "The package is damaged (" + target + ")."; return false; }
        files[target] = std::move(raw);
    }
    return true;
}

const Bytes* FindFile(const std::map<std::string, Bytes>& files, const std::string& suffix) {
    for (const auto& [name, data] : files)
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) return &data;
    return nullptr;
}

// ---- mgs_ngi.s3e: LZMA ("LZMA alone": 5 property bytes, u64 size; trailing bytes after the stream) -------------
bool UnpackS3e(const Bytes& in, Bytes& out) {
    if (in.size() < 13) return false;
    uint64_t size; memcpy(&size, in.data() + 5, 8);
    if (size > 64u * 1024 * 1024) return false;
    out.resize(static_cast<size_t>(size));
    SizeT dl = out.size(), sl = in.size() - 13;
    ELzmaStatus status;
    ISzAlloc alloc = { [](ISzAllocPtr, size_t s) -> void* { return malloc(s); }, [](ISzAllocPtr, void* p) { free(p); } };
    const SRes r = LzmaDecode(out.data(), &dl, in.data() + 13, &sl, in.data(), LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &status, &alloc);
    return r == SZ_OK && dl == out.size();
}

// ---- Derbh archives (.dz) through the game's own code -----------------------------------------------------
// The "dz" compression is Ideaworks' own, so the archives are unpacked by the game's CDerbhArchive in a CPU
// instance of its own (Unicorn; the native backend on the 3DS, which has no Unicorn), with the same calls
// tools/game_dz_extract.py makes (README.md §6.2): ctor 0x4A07D690, Attach 0x4A07D084(obj, name, 0, 0), open
// 0x4A07D33C(obj, name, "rb"), read 0x4A07D470(obj, buf, 1, size, handle), close 0x4A07D32C(obj, handle). Its
// imports (s3eFile*, s3eMalloc*) are serviced here; Derbh's allocator hooks [0x4A0EDB90+8 / +0xC] point at two
// extra import trampolines (alloc, free) after the game's. One instance serves every archive (the native
// backend's memory comes back only when it's destroyed); each archive starts with an empty heap.
class DzRunner {
public:
    explicit DzRunner(const S3EBinary& bin) : bin_(bin) {}

    bool Extract(const Bytes& archive, const std::function<bool(const std::string&, const Bytes&)>& sink, std::string* error) {
        archive_ = &archive;
        if (!uc_ && !Init()) { *error = "could not start the archive reader"; return false; }
        heap_ = HEAP_BASE + 0x100;
        sizes_.clear();
        files_.clear();
        failed_ = false;
        std::vector<std::string> names = EntryNames(archive);
        if (names.empty()) { *error = "the archive has no files"; return false; }
        const uint32_t obj = Alloc(0x44C);
        Call(G(0x4A07D690), {obj});
        if (Call(G(0x4A07D084), {obj, PutString("archive.dz"), 0, 0}) != 0 || failed_) { *error = "the game could not open the archive"; return false; }
        const uint32_t rb = PutString("rb");
        constexpr uint32_t kChunk = 0x10000;
        const uint32_t buf = Alloc(kChunk);
        for (const std::string& n : names) {
            std::string game_name = n;
            for (char& c : game_name) c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
            const uint32_t h = Call(G(0x4A07D33C), {obj, PutString(game_name), rb});
            if (!h || failed_) { *error = "could not read " + n + " from the archive"; return false; }
            Bytes data;
            for (;;) {
                const uint32_t got = Call(G(0x4A07D470), {obj, buf, 1, kChunk, h});
                if (failed_ || got == 0 || got > kChunk) break;
                const size_t at = data.size();
                data.resize(at + got);
                uc_->MemRead(buf, data.data() + at, got);
                if (got < kChunk) break;
            }
            Call(G(0x4A07D32C), {obj, h});
            if (failed_) { *error = "the archive reader stopped at " + n; return false; }
            if (!sink(n, data)) { *error = "could not write " + n; return false; }
        }
        std::cout << "[Setup] " << names.size() << " files, guest heap used " << (heap_ - HEAP_BASE) / 1024 << " KB of "
                  << HEAP_SIZE / 1024 << std::endl;
        return true;
    }

    // Paths from the archive's string table and directory tree (DTRZ: u16 file count, u16 dir count at +4,
    // names from +9; then per file a list of u16 dir ids ending with 0xFFFF).
    static std::vector<std::string> EntryNames(const Bytes& d) {
        std::vector<std::string> out;
        if (d.size() < 9 || memcmp(d.data(), "DTRZ", 4) != 0) return out;
        const int nf = d[4] | d[5] << 8, nd = d[6] | d[7] << 8;
        size_t p = 9;
        std::vector<std::string> strs;
        for (int i = 0; i < nf + nd - 1; ++i) {
            size_t e = p;
            while (e < d.size() && d[e]) ++e;
            if (e >= d.size()) return {};
            strs.emplace_back(reinterpret_cast<const char*>(&d[p]), e - p);
            p = e + 1;
        }
        std::vector<std::string> dirs = {""};
        dirs.insert(dirs.end(), strs.begin() + nf, strs.end());
        for (int i = 0; i < nf; ++i) {
            std::vector<int> ids;
            for (;;) {
                if (p + 2 > d.size()) return {};
                const int v = d[p] | d[p + 1] << 8; p += 2;
                if (v == 0xFFFF) break;
                ids.push_back(v);
            }
            const std::string dir = !ids.empty() && ids[0] < static_cast<int>(dirs.size()) ? dirs[ids[0]] : "";
            out.push_back(dir.empty() ? strs[i] : dir + "\\" + strs[i]);
        }
        return out;
    }

private:
    // Where a call returns to: Unicorn stops when the PC gets there (its page is mapped for that); the native
    // backend ends the run when LR holds exactly this value at the start (it swaps in an exit stub).
    static constexpr uint32_t kReturn = 0x7F000000;

    bool Init() {
#if defined(MGS_3DS) || defined(MGS_VITA)   // (no Unicorn on the consoles)
        cpu_ = Cpu::CreateNative(nullptr);
#else
        cpu_ = Cpu::CreateUnicorn(nullptr);
#endif
        // 4 MB of guest heap: Derbh needs ~200 KB per archive here (it frees nothing; each archive starts over)
        if (!cpu_ || !SetupSandbox(cpu_.get(), bin_, STOCK_BASE, 4 * 1024 * 1024)) { cpu_.reset(); return false; }
        uc_ = cpu_.get();
        if (!uc_->IsNative()) uc_->MemMap(kReturn, 0x1000);
        // Derbh's allocator hooks: two trampolines after the game's imports (ARM code, so plain pointers)
        alloc_index_ = static_cast<uint32_t>(bin_.imports.size());
        const uint32_t alloc_fn = TRAMPOLINE_BASE + alloc_index_ * 8, free_fn = alloc_fn + 8;
        uc_->WriteTrampoline(alloc_fn, alloc_index_);
        uc_->WriteTrampoline(free_fn, alloc_index_ + 1);
        uc_->MemWrite(G(0x4A0EDB90) + 8, &alloc_fn, 4);
        uc_->MemWrite(G(0x4A0EDB90) + 12, &free_fn, 4);
        uc_->AddSvcHook(+[](Cpu* uc, uint32_t, void* self) {
            static_cast<DzRunner*>(self)->OnSvc(uc);
        }, this);
        return true;
    }

    uint32_t Alloc(uint32_t n) {
        const uint32_t a = (heap_ + 15) & ~15u;
        n = (std::max<uint32_t>)(n, 4u);
        if (a + n + 16 > HEAP_BASE + HEAP_SIZE) { failed_ = true; return 0; }
        heap_ = a + n + 16;
        static const Bytes zero(0x10000, 0);
        for (uint32_t o = 0; o < n; o += 0x10000) uc_->MemWrite(a + o, zero.data(), (std::min<uint32_t>)(n - o, 0x10000u));
        sizes_[a] = n;
        return a;
    }

    uint32_t PutString(const std::string& s) {
        const uint32_t a = Alloc(static_cast<uint32_t>(s.size()) + 1);
        uc_->MemWrite(a, s.c_str(), s.size() + 1);
        return a;
    }

    std::string GuestString(uint32_t a) {
        std::string s; char c;
        while (s.size() < 1024 && uc_->MemRead(a + static_cast<uint32_t>(s.size()), &c, 1) == Cpu::OK && c) s += c;
        return s;
    }

    uint32_t Call(uint32_t fn, std::initializer_list<uint32_t> args) {
        std::vector<uint32_t> a(args);
        uint32_t sp = STACK_BASE + STACK_SIZE - 0x100;
        if (a.size() > 4) {
            sp -= 4 * static_cast<uint32_t>(a.size() - 4);
            uc_->MemWrite(sp, a.data() + 4, 4 * (a.size() - 4));
        }
        static const int regs[4] = {Cpu::REG_R0, Cpu::REG_R1, Cpu::REG_R2, Cpu::REG_R3};
        for (size_t i = 0; i < 4; ++i) { uint32_t v = i < a.size() ? a[i] : 0; uc_->RegWrite(regs[i], &v); }
        const bool native = uc_->IsNative();
        const uint32_t lr = native ? kReturn : kReturn | 1;
        uc_->RegWrite(Cpu::REG_SP, &sp);
        uc_->RegWrite(Cpu::REG_LR, &lr);
        if (uc_->Start(fn | 1, kReturn, 400000000) != Cpu::OK) { failed_ = true; return 0; }
        uint32_t pc = 0, r0 = 0;
        uc_->RegRead(Cpu::REG_PC, &pc);
        if (!native && (pc & ~1u) != kReturn) { failed_ = true; return 0; }   // ran out of instructions (Unicorn)
        uc_->RegRead(Cpu::REG_R0, &r0);
        return r0;
    }

    struct File { const Bytes* data; size_t pos; };

    void OnSvc(Cpu* uc) {
        uint32_t pc = 0, r[4];
        uc->RegRead(Cpu::REG_PC, &pc);
        static const int regs[4] = {Cpu::REG_R0, Cpu::REG_R1, Cpu::REG_R2, Cpu::REG_R3};
        for (int i = 0; i < 4; ++i) uc->RegRead(regs[i], &r[i]);
        std::string name;
        const uint32_t idx = (pc - 4 - TRAMPOLINE_BASE) / 8;
        if (idx == alloc_index_) name = "dzAlloc";
        else if (idx == alloc_index_ + 1) name = "dzFree";
        else if (idx < bin_.imports.size()) name = bin_.imports[idx];
        uint32_t ret = 0;
        if (name == "dzAlloc" || name == "s3eMalloc" || name == "s3eMallocBase") ret = Alloc(r[0]);
        else if (name == "s3eRealloc" || name == "s3eReallocBase") {
            ret = Alloc(r[1]);
            auto old = sizes_.find(r[0]);
            if (r[0] && old != sizes_.end()) {
                Bytes tmp((std::min)(old->second, r[1]));
                uc->MemRead(r[0], tmp.data(), tmp.size());
                uc->MemWrite(ret, tmp.data(), tmp.size());
            }
        } else if (name == "s3eFileOpen" || name == "s3eFileCheckExists") {
            if (GuestString(r[0]) == "archive.dz") {
                if (name == "s3eFileCheckExists") ret = 1;
                else { ret = next_handle_++; files_[ret] = {archive_, 0}; }
            }
        } else if (name == "s3eFileRead") {   // buf, size, count, handle
            auto f = files_.find(r[3]);
            if (f != files_.end() && r[1]) {
                const size_t want = static_cast<size_t>(r[1]) * r[2];
                const size_t got = (std::min)(want, f->second.data->size() - (std::min)(f->second.pos, f->second.data->size()));
                if (got) uc->MemWrite(r[0], f->second.data->data() + f->second.pos, got);
                f->second.pos += got;
                ret = static_cast<uint32_t>(got / r[1]);
            }
        } else if (name == "s3eFileSeek") {   // handle, offset, origin (0 set, 1 current, 2 end)
            auto f = files_.find(r[0]);
            if (f == files_.end()) ret = 1;
            else {
                const int64_t base = r[2] == 1 ? static_cast<int64_t>(f->second.pos) : r[2] == 2 ? static_cast<int64_t>(f->second.data->size()) : 0;
                f->second.pos = static_cast<size_t>((std::max)(int64_t{0}, base + static_cast<int32_t>(r[1])));
            }
        } else if (name == "s3eFileTell") {
            auto f = files_.find(r[0]);
            ret = f == files_.end() ? 0xFFFFFFFFu : static_cast<uint32_t>(f->second.pos);
        } else if (name == "s3eFileGetSize") {
            auto f = files_.find(r[0]);
            ret = f == files_.end() ? 0xFFFFFFFFu : static_cast<uint32_t>(f->second.data->size());
        } else if (name == "s3eFileEOF") {
            auto f = files_.find(r[0]);
            ret = f == files_.end() || f->second.pos >= f->second.data->size();
        } else if (name == "s3eFileGetChar") {
            auto f = files_.find(r[0]);
            ret = 0xFFFFFFFFu;
            if (f != files_.end() && f->second.pos < f->second.data->size()) ret = (*f->second.data)[f->second.pos++];
        } else if (name == "s3eFileClose") {
            files_.erase(r[0]);
        }   // anything else (s3eFree, s3eConfigGetInt, ...) returns 0
        uc->RegWrite(Cpu::REG_R0, &ret);
        // Back to the caller, as the runtime does (runtime.cpp): the native backend's trampoline is only `b stub`,
        // with no `bx lr` after it as under Unicorn. PC first (it resets the Thumb state), then LR's Thumb bit.
        uint32_t lr = 0, cpsr = 0;
        uc->RegRead(Cpu::REG_LR, &lr);
        const uint32_t to = lr & ~1u;
        uc->RegWrite(Cpu::REG_PC, &to);
        uc->RegRead(Cpu::REG_CPSR, &cpsr);
        cpsr = (lr & 1) ? (cpsr | 0x20) : (cpsr & ~0x20u);
        uc->RegWrite(Cpu::REG_CPSR, &cpsr);
    }

    const S3EBinary& bin_;
    const Bytes* archive_ = nullptr;
    std::unique_ptr<Cpu> cpu_;
    Cpu* uc_ = nullptr;
    uint32_t heap_ = 0;
    uint32_t alloc_index_ = 0;
    uint32_t next_handle_ = 0x1001;
    bool failed_ = false;
    std::map<uint32_t, uint32_t> sizes_;
    std::map<uint32_t, File> files_;
};

fs::path ExeDir(const std::string& exe_dir) { return fs::u8path(exe_dir); }

// Deletes a folder and everything in it: lists it all first, then deletes (files, then folders, deepest first).
// fs::remove_all deletes while it iterates, which never finished on the 3DS's SD card (devkitARM).
void RemoveTree(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    std::vector<fs::path> files, dirs;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        (it->is_directory(ec) ? dirs : files).push_back(it->path());
    for (const fs::path& f : files) fs::remove(f, ec);
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    for (const fs::path& d : dirs) fs::remove(d, ec);
    fs::remove(dir, ec);
}

// Moves everything from staging into dest (keeping files only dest has, such as saves); the binary goes
// last because its presence marks an installed copy.
bool MoveTree(const fs::path& staging, const fs::path& dest, std::string* error) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(staging, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) files.push_back(it->path());
    std::stable_partition(files.begin(), files.end(), [](const fs::path& p) { return p.filename() != "mgs_ngi"; });
    // The path below staging as text: fs::relative goes through weakly_canonical, which didn't come back on the
    // 3DS (its root is the device name "sdmc:").
    const std::string base = staging.generic_u8string();
    size_t moved = 0;
    for (const fs::path& f : files) {
        std::string rel = f.generic_u8string().substr(base.size());
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        const fs::path to = dest / fs::u8path(rel);
        fs::create_directories(to.parent_path(), ec);
        fs::remove(to, ec);      // (the 3DS doesn't rename onto an existing file)
        ec.clear();
        fs::rename(f, to, ec);
        if (++moved % 50 == 0) std::cout << "[Setup] Moved " << moved << " of " << files.size() << " files" << std::endl;
        if (ec) {                // e.g. another drive: copy, then delete
            ec.clear();
            if (!fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec) || ec) {
                *error = "Could not move " + f.u8string() + " into the game folder.";
                return false;
            }
            fs::remove(f, ec);
        }
    }
    RemoveTree(staging);
    return true;
}
}

bool Installer::Installed(const std::string& exe_dir) {
    std::error_code ec;
    return fs::exists(ExeDir(exe_dir) / "game" / "mgs_ngi", ec) && fs::exists(ExeDir(exe_dir) / "game" / "data" / "settings.group.bin", ec);
}

bool Installer::Install(const fs::path& package, const std::string& exe_dir,
                        const std::function<void(float, const std::string&)>& progress, std::string* error) {
    auto fail = [&](const std::string& e) { *error = e; std::cout << "[Setup] " << e << std::endl; return false; };
    auto step = [&](float f, const std::string& s) { std::cout << "[Setup] " << s << std::endl; if (progress) progress(f, s); };
    const fs::path root = ExeDir(exe_dir);
    const fs::path staging = root / "game.setup";
    std::error_code ec;

    step(0.02f, "Reading the package...");
    Bytes pkg;
    if (!ReadAll(package, pkg)) return fail("Could not open the file.");
    std::map<std::string, Bytes> files;
    if (!ReadPackage(pkg, files, error)) return fail(*error);
    pkg.clear();
    const Bytes* all = FindFile(files, "\\private\\20010925\\all.dz");
    const Bytes* warp = FindFile(files, "\\private\\20010925\\warp.dz");
    const Bytes* s3e = FindFile(files, "\\private\\20010925\\mgs_ngi.s3e");
    const Bytes* scrambled = FindFile(files, "\\private\\20010925\\bin\\encrypted.dz.ngdat");
    if (!all || !warp || !s3e) return fail("This package isn't Metal Gear Solid Mobile (the game files are missing).");
    if (Crc(*s3e) != kCrcS3e || Crc(*all) != kCrcAllDz || Crc(*warp) != kCrcWarpDz)
        return fail("This version of Metal Gear Solid Mobile isn't supported (CRC " + Hex(Crc(*s3e)) + " / " + Hex(Crc(*all)) + ").");
    if (!scrambled || Crc(*scrambled) != kCrcScrambled)
        return fail("This copy lacks the full-version data (bin\\encrypted.dz.ngdat), which Area 3 and the VR missions need.");

    RemoveTree(staging);
    step(0.08f, "Unpacking the game program...");
    Bytes binary;
    if (!UnpackS3e(*s3e, binary) || Crc(binary) != kCrcBinary) return fail("Could not unpack the game program (mgs_ngi.s3e).");
    if (!WriteAll(staging / "mgs_ngi", binary)) return fail("Could not write to " + staging.string() + ".");
    S3EBinary bin;
    if (!LoadS3E((staging / "mgs_ngi").u8string(), bin)) return fail("Could not load the game program.");

    Bytes scripts = *scrambled;
    for (const XorRun& x : kScriptsXor)
        for (int i = 0; i < x.len; ++i) scripts[x.offset + i] ^= x.x[i];
    if (Crc(scripts) != kCrcScripts) return fail("Could not restore the full-version scripts.");

    // The game data: all.dz and the scripts into data\ (README.md §6.2, §6.11), warp.dz into warp\, and both
    // archives beside data\ as on the phone (the game checks that all.dz exists).
    struct Job { const Bytes* archive; fs::path dir; const char* what; float from, to; };
    const Job jobs[] = {
        {all, staging / "data", "Extracting the game data", 0.12f, 0.72f},
        {&scripts, staging / "data", "Extracting the full-version scripts", 0.72f, 0.78f},
        {warp, staging / "warp", "Extracting the warp data", 0.78f, 0.80f},
    };
    DzRunner dz(bin);
    for (const Job& j : jobs) {
        step(j.from, std::string(j.what) + "...");
        const size_t total = DzRunner::EntryNames(*j.archive).size();
        size_t done = 0;
        std::string err;
        const bool ok = dz.Extract(*j.archive, [&](const std::string& name, const Bytes& data) {
            fs::path p = j.dir;
            size_t s = 0, e;
            while ((e = name.find('\\', s)) != std::string::npos) { p /= fs::u8path(name.substr(s, e - s)); s = e + 1; }
            p /= fs::u8path(name.substr(s));
            if (!WriteAll(p, data)) return false;
            ++done;
            if (progress && total) progress(j.from + (j.to - j.from) * done / total, std::string(j.what) + " (" + std::to_string(done) + "/" + std::to_string(total) + ")");
            return true;
        }, &err);
        if (!ok) return fail(std::string(j.what) + " failed: " + err + ".");
    }
    if (!WriteAll(staging / "all.dz", *all) || !WriteAll(staging / "warp.dz", *warp)) return fail("Could not write the archives.");
    files.clear();

    step(0.82f, "Installing...");
    if (!MoveTree(staging, root / "game", error)) return fail(*error);
    {   // a pack of the previous files (3DS, vfs.cpp) would no longer match: rebuilt on the next start
        std::error_code ec;
        fs::remove(root / "game" / "data.pak", ec);
    }

#if !defined(MGS_3DS) && !defined(MGS_VITA)   // (consoles: no PNGs; custom textures are optional and would take long to write to the card)
    const fs::path tex = root / "textures";
    if (!TexturePack::Extracted(tex)) {
        step(0.84f, "Extracting textures...");
        std::string err;
        const int n = TexturePack::Extract((root / "game" / "data").u8string(), tex, [&](int d, int t) {
            if (progress && t) progress(0.84f + 0.16f * d / t, "Extracting textures (" + std::to_string(d) + "/" + std::to_string(t) + ")");
        }, &err);
        if (n < 0) std::cout << "[Setup] Texture extraction failed: " << err << std::endl;   // not fatal: the game runs without
    }
#endif
    step(1.0f, "Setup complete.");
    return true;
}
