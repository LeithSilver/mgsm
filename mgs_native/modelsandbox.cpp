#include "modelsandbox.h"
#include "cpu.h"
#include "gles.h"
#include "loader.h"
#include "runtime.h"
#include "settings.h"
#include "vfs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>

namespace {
// Guest functions (README.md §6.23)
constexpr uint32_t kLoadGroup = 0x4A02648C;       // (path, 0): CIwResManager loads a group (".bin" added)
constexpr uint32_t kResManager = 0x4A0F0270;      // CIwResManager*; +0x54 = the group just loaded
constexpr uint32_t kResolveGroup = 0x4A066814;   // (group, 1): flag 2, group vtable +0x18 (resolves references)
constexpr uint32_t kGetListNamed = 0x4A0BE564;    // (group, class name, 1) -> CIwResList*, its CIwManagedList at +0xC
constexpr uint32_t kSetModelMatrix = 0x4A030BC0;  // IwGxSetModelMatrix(const CIwMat*, bool), Thumb veneer
constexpr uint32_t kSetViewMatrix = 0x4A02D2F0;   // IwGxSetViewMatrix(const CIwMat*), Thumb veneer
constexpr uint32_t kFlush = 0x4A04C748;           // IwGxFlush(), Thumb veneer
constexpr uint32_t kTextureUpload = 0x4A0607B0;   // CIwTexture upload (r0 = the texture, hash at +4)
constexpr uint32_t kTextureUploadEnd = 0x4A060E28;
constexpr uint32_t kModelRenderSlot = 0x38;       // CIwModel vtable: Render(model, bool, flags)
constexpr uint32_t kAnimContext = 0x4A0EDBAC;     // IwAnim skinning context: +4 skeleton, +8 CIwAnimSkin (0x4A0B8906)
constexpr uint32_t kAnimFrame = 0x4A0F13A8;       // (CAnimated::Render puts it in context +0x10)

constexpr uint32_t kAllocBase = 0x58000000, kAllocSize = 64u << 20;   // the copy's own allocations
constexpr uint32_t kReturn = 0x7F000000;

// GL enums (ES 1.1)
enum : uint32_t {
    kPoints = 0, kTriangles = 4, kTriangleStrip = 5, kTriangleFan = 6,
    kByte = 0x1400, kUByte = 0x1401, kShort = 0x1402, kUShort = 0x1403, kFloat = 0x1406, kFixed = 0x140C,
    kModelview = 0x1700, kProjection = 0x1701, kTextureMatrix = 0x1702,
    kTexture2D = 0x0DE1, kBlend = 0x0BE2, kAlphaTest = 0x0BC0,
    kVertexArray = 0x8074, kNormalArray = 0x8075, kColorArray = 0x8076, kTexCoordArray = 0x8078,
    kTexture0 = 0x84C0, kUnpackAlignment = 0x0CF5,
    kAlpha = 0x1906, kRGB = 0x1907, kRGBA = 0x1908, kLuminance = 0x1909, kLuminanceAlpha = 0x190A,
    k4444 = 0x8033, k5551 = 0x8034, k565 = 0x8363,
    kPalette4RGB8 = 0x8B90, kPalette8RGB5A1 = 0x8B99,
};

struct Array { bool on = false; int size = 4; uint32_t type = kFloat, stride = 0, ptr = 0; };

void Identity(float* m) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
void Mul(float* m, const float* b) {   // m = m * b (column-major, as glMultMatrix)
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = m[row] * b[c * 4] + m[4 + row] * b[c * 4 + 1] + m[8 + row] * b[c * 4 + 2] + m[12 + row] * b[c * 4 + 3];
    memcpy(m, r, sizeof(r));
}
float Fx(uint32_t v) { return static_cast<int32_t>(v) / 65536.0f; }

class Sandbox {
public:
    ~Sandbox() { for (uint32_t h : disk_files_) VirtualFileSystem::Instance().Close(h); }

    bool Init(std::string* error) {
        S3ERuntime& rt = S3ERuntime::Instance();
        Cpu* live = rt.GuestCpu();
        imports_ = &rt.Imports();
        if (!live) { *error = "the game isn't running"; return false; }
        cpu_ = Cpu::CreateUnicorn(error);
        uc_ = cpu_.get();
        if (!uc_) return false;
        // The image's mapped size: pages until the first unreadable one.
        uint32_t image = 0;
        for (uint32_t probe; image < (8u << 20) && live->MemRead(ImageBase() + image, &probe, 4) == Cpu::OK; image += 0x1000) {}
        const struct { uint32_t base, size; } regions[] = {
            {TRAMPOLINE_BASE, 0x10000}, {ImageBase(), image}, {STACK_BASE, STACK_SIZE}, {FB_BASE, FB_SIZE}, {HEAP_BASE, HEAP_SIZE},
        };
        std::vector<uint8_t> buf(1u << 20);
        for (const auto& r : regions) {
            if (uc_->MemMap(r.base, r.size) != Cpu::OK) { *error = "could not map the copy's memory"; return false; }
            for (uint32_t o = 0; o < r.size; o += static_cast<uint32_t>(buf.size())) {
                const uint32_t n = (std::min)(static_cast<uint32_t>(buf.size()), r.size - o);
                if (live->MemRead(r.base + o, buf.data(), n) != Cpu::OK) continue;
                if (std::all_of(buf.begin(), buf.begin() + n, [](uint8_t b) { return b == 0; })) continue;   // already zero
                uc_->MemWrite(r.base + o, buf.data(), n);
            }
        }
        uc_->MemMap(kAllocBase, kAllocSize);
        uc_->MemMap(kReturn, 0x1000);
        uint32_t sp = 0;
        live->RegRead(Cpu::REG_SP, &sp);
        sp_ = (sp - 0x4000) & ~7u;   // below the paused game's frames, which stay intact in the copy
        if (sp_ < STACK_BASE + 0x20000) sp_ = STACK_BASE + STACK_SIZE - 0x100;
        alloc_ = kAllocBase;
        uc_->AddSvcHook(+[](Cpu* uc, uint32_t, void* self) { static_cast<Sandbox*>(self)->OnSvc(uc); }, this);
        uc_->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
            uint32_t tex = 0, hash = 0;
            uc->RegRead(Cpu::REG_R0, &tex);
            if (tex && uc->MemRead(tex + 4, &hash, 4) == Cpu::OK) static_cast<Sandbox*>(self)->upload_hash_ = hash;
        }, this, G(kTextureUpload), G(kTextureUpload));
        Identity(mv_.back().m); Identity(proj_.back().m); Identity(texm_.back().m);
        // IwGx only sends GL what changed, so start from the state the game's last frame left.
        const GLES::GuestState& g = GLES::Instance().PausedState();
        if (g.valid) {
            matrix_mode_ = g.matrix_mode;
            memcpy(mv_.back().m, g.mv, sizeof(g.mv));
            memcpy(proj_.back().m, g.proj, sizeof(g.proj));
            memcpy(texm_.back().m, g.tex, sizeof(g.tex));
            tex2d_[0] = g.tex2d[0]; tex2d_[1] = g.tex2d[1];
            blend_ = g.blend; alpha_test_ = g.alpha_test;
            bound_[0] = g.bound[0]; bound_[1] = g.bound[1];
            unit_ = g.active_unit & 1; client_unit_ = g.client_unit;
            auto arr = [](const GLES::GuestState::Array& a) { return Array{a.on, a.size, a.type, a.stride, a.ptr}; };
            vertex_ = arr(g.vertex); color_ = arr(g.color); texcoord_[0] = arr(g.texcoord[0]); texcoord_[1] = arr(g.texcoord[1]);
            memcpy(color4_, g.color4, sizeof(color4_));
            unpack_ = static_cast<uint32_t>((std::max)(1, g.unpack_align));
        }
        return true;
    }

    uint32_t Alloc(uint32_t n) {
        const uint32_t a = (alloc_ + 15) & ~15u;
        n = (std::max<uint32_t>)(n, 4u);
        if (a + n > kAllocBase + kAllocSize) { failed_ = "the copy ran out of memory"; return 0; }
        alloc_ = a + n;
        sizes_[a] = n;
        return a;   // (mapped memory starts zeroed and is never reused)
    }

    uint32_t PutString(const std::string& s) {
        const uint32_t a = Alloc(static_cast<uint32_t>(s.size()) + 1);
        if (a) uc_->MemWrite(a, s.c_str(), s.size() + 1);
        return a;
    }

    uint32_t Call(uint32_t fn, std::initializer_list<uint32_t> args) {   // fn: bit 0 = Thumb
        if (!failed_.empty()) return 0;
        std::vector<uint32_t> a(args);
        uint32_t sp = sp_;
        if (a.size() > 4) {
            sp -= 4 * static_cast<uint32_t>(a.size() - 4);
            uc_->MemWrite(sp, a.data() + 4, 4 * (a.size() - 4));
        }
        static const int regs[4] = {Cpu::REG_R0, Cpu::REG_R1, Cpu::REG_R2, Cpu::REG_R3};
        for (size_t i = 0; i < 4; ++i) { uint32_t v = i < a.size() ? a[i] : 0; uc_->RegWrite(regs[i], &v); }
        const uint32_t lr = kReturn | 1;
        uc_->RegWrite(Cpu::REG_SP, &sp);
        uc_->RegWrite(Cpu::REG_LR, &lr);
        const int err = uc_->Start(fn, kReturn, 500000000);
        uint32_t pc = 0, r0 = 0;
        uc_->RegRead(Cpu::REG_PC, &pc);
        if (err != Cpu::OK || (pc & ~1u) != kReturn) {
            char b[160];
            snprintf(b, sizeof b, "the game's code stopped at 0x%08x (%s) in 0x%08x", pc, err != Cpu::OK ? uc_->ErrorText(err) : "out of time", fn);
            failed_ = b;
            return 0;
        }
        uc_->RegRead(Cpu::REG_R0, &r0);
        return r0;
    }

    uint32_t Read32(uint32_t a) { uint32_t v = 0; uc_->MemRead(a, &v, 4); return v; }

    Cpu* uc_ = nullptr;
    std::string failed_;
    std::vector<std::string> files_;
    // The recorder: draws are appended to `draws_`, textures by sandbox GL name.
    std::vector<ModelSandbox::Draw> draws_;
    struct Tex { uint32_t hash = 0; int w = 0, h = 0; std::vector<uint32_t> rgba; };
    std::map<uint32_t, Tex> textures_;

private:
    struct Mat { float m[16]; };

    void OnSvc(Cpu* uc) {
        uint32_t pc = 0, r[4];
        uc->RegRead(Cpu::REG_PC, &pc);
        static const int regs[4] = {Cpu::REG_R0, Cpu::REG_R1, Cpu::REG_R2, Cpu::REG_R3};
        for (int i = 0; i < 4; ++i) uc->RegRead(regs[i], &r[i]);
        const uint32_t idx = (pc - 4 - TRAMPOLINE_BASE) / 8;
        const std::string& name = idx < imports_->size() ? (*imports_)[idx] : empty_;
        uint32_t ret = 0;
        if (name.compare(0, 2, "gl") == 0) ret = Gl(name, r);
        else if (name.compare(0, 7, "s3eFile") == 0) ret = File(name, r);
        else if (name == "s3eMalloc" || name == "s3eMallocBase") ret = Alloc(r[0]);
        else if (name == "s3eRealloc" || name == "s3eReallocBase") {
            ret = Alloc(r[1]);
            auto old = sizes_.find(r[0]);
            if (ret && r[0]) {
                // Old blocks may come from the game's own heap (copied); copy what fits either way.
                const uint32_t n = old != sizes_.end() ? (std::min)(old->second, r[1]) : r[1];
                std::vector<uint8_t> tmp(n);
                uc->MemRead(r[0], tmp.data(), n);
                uc->MemWrite(ret, tmp.data(), n);
            }
        } else if (name == "s3eTimerGetMs") {
            ret = static_cast<uint32_t>(S3ERuntime::Instance().GameMs());
        }   // anything else (frees, config, debug output, ...) returns 0
        uc->RegWrite(Cpu::REG_R0, &ret);
    }

    uint32_t Arg(const uint32_t r[4], int i) {
        if (i < 4) return r[i];
        uint32_t sp = 0;
        uc_->RegRead(Cpu::REG_SP, &sp);
        return Read32(sp + 4 * (i - 4));
    }

    std::string GuestString(uint32_t a) {
        std::string s; char c;
        while (a && s.size() < 1024 && uc_->MemRead(a + static_cast<uint32_t>(s.size()), &c, 1) == Cpu::OK && c) s += c;
        return s;
    }

    // ---- s3eFile*: disk files through the game's VFS (read only), memory files here ----
    struct MemFile { uint32_t addr, size, pos; };
    uint32_t File(const std::string& name, const uint32_t r[4]) {
        VirtualFileSystem& vfs = VirtualFileSystem::Instance();
        if (name == "s3eFileCheckExists") return vfs.CheckExists(GuestString(r[0])) ? 1 : 0;
        if (name == "s3eFileOpen") {
            const std::string path = GuestString(r[0]), mode = GuestString(r[1]);
            if (mode.find_first_of("wa+") != std::string::npos) return 0;   // never write from the copy
            const uint32_t h = vfs.Open(path, mode);
            if (h) { disk_files_.insert(h); files_.push_back(path); }
            return h;
        }
        if (name == "s3eFileOpenFromMemory") {
            const uint32_t h = next_mem_++;
            mem_files_[h] = {r[0], r[1], 0};
            return h;
        }
        auto mem = mem_files_.find(name == "s3eFileRead" ? r[3] : r[0]);
        const bool is_mem = mem != mem_files_.end();
        if (name == "s3eFileRead") {   // buf, size, count, handle
            if (!r[1] || !r[2]) return 0;
            const size_t want = static_cast<size_t>(r[1]) * r[2];
            std::vector<uint8_t> tmp(want);
            uint32_t got = 0;
            if (is_mem) {
                MemFile& f = mem->second;
                const uint32_t n = static_cast<uint32_t>((std::min)(want, static_cast<size_t>(f.size - (std::min)(f.pos, f.size))));
                uc_->MemRead(f.addr + f.pos, tmp.data(), n);
                f.pos += n;
                got = n / r[1];
                uc_->MemWrite(r[0], tmp.data(), static_cast<size_t>(got) * r[1]);
                return got;
            }
            if (!disk_files_.count(r[3])) return 0;
            got = vfs.Read(r[3], tmp.data(), r[1], r[2]);
            if (got) uc_->MemWrite(r[0], tmp.data(), static_cast<size_t>(got) * r[1]);
            return got;
        }
        if (name == "s3eFileSeek") {
            if (is_mem) {
                MemFile& f = mem->second;
                const int64_t base = r[2] == 1 ? f.pos : r[2] == 2 ? f.size : 0;
                f.pos = static_cast<uint32_t>((std::max)(int64_t{0}, base + static_cast<int32_t>(r[1])));
                return 0;
            }
            return disk_files_.count(r[0]) ? static_cast<uint32_t>(vfs.Seek(r[0], static_cast<int32_t>(r[1]), static_cast<int>(r[2]))) : 1;
        }
        if (name == "s3eFileTell") return is_mem ? mem->second.pos : disk_files_.count(r[0]) ? static_cast<uint32_t>(vfs.Tell(r[0])) : 0xFFFFFFFFu;
        if (name == "s3eFileGetSize") return is_mem ? mem->second.size : disk_files_.count(r[0]) ? vfs.GetSize(r[0]) : 0xFFFFFFFFu;
        if (name == "s3eFileEOF") return is_mem ? (mem->second.pos >= mem->second.size) : disk_files_.count(r[0]) ? vfs.EOF_Reached(r[0]) : 1;
        if (name == "s3eFileGetChar") {
            if (is_mem) {
                MemFile& f = mem->second;
                if (f.pos >= f.size) return 0xFFFFFFFFu;
                uint8_t c = 0; uc_->MemRead(f.addr + f.pos++, &c, 1); return c;
            }
            return disk_files_.count(r[0]) ? static_cast<uint32_t>(vfs.GetChar(r[0])) : 0xFFFFFFFFu;
        }
        if (name == "s3eFileClose") {
            if (is_mem) mem_files_.erase(mem);
            else if (disk_files_.erase(r[0])) vfs.Close(r[0]);
            return 0;
        }
        return 0;   // writes, deletes, directories: refused
    }

    // ---- GL ES 1.1, recorded ----
    std::vector<Mat>& Stack() { return matrix_mode_ == kProjection ? proj_ : matrix_mode_ == kTextureMatrix ? texm_ : mv_; }

    uint32_t Gl(const std::string& n, const uint32_t r[4]) {
        if (n == "glMatrixMode") { matrix_mode_ = r[0]; return 0; }
        if (n == "glLoadIdentity") { Identity(Stack().back().m); return 0; }
        if (n == "glLoadMatrixx") { for (int i = 0; i < 16; ++i) Stack().back().m[i] = Fx(Read32(r[0] + 4 * i)); return 0; }
        if (n == "glPushMatrix") { Stack().push_back(Stack().back()); return 0; }
        if (n == "glPopMatrix") { if (Stack().size() > 1) Stack().pop_back(); return 0; }
        if (n == "glTranslatex") { float t[16]; Identity(t); t[12] = Fx(r[0]); t[13] = Fx(r[1]); t[14] = Fx(r[2]); Mul(Stack().back().m, t); return 0; }
        if (n == "glScalex") { float t[16]; Identity(t); t[0] = Fx(r[0]); t[5] = Fx(r[1]); t[10] = Fx(r[2]); Mul(Stack().back().m, t); return 0; }
        if (n == "glRotatex") {
            float x = Fx(r[1]), y = Fx(r[2]), z = Fx(r[3]);
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len > 0) { x /= len; y /= len; z /= len; }
            const float a = Fx(r[0]) * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
            const float m[16] = { t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,  t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
                                  t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,  0, 0, 0, 1 };
            Mul(Stack().back().m, m);
            return 0;
        }
        if (n == "glFrustumx" || n == "glOrthox") {
            const float l = Fx(r[0]), rt = Fx(r[1]), b = Fx(r[2]), t = Fx(r[3]), nz = Fx(Arg(r, 4)), fz = Fx(Arg(r, 5));
            float m[16] = {};
            if (n == "glFrustumx") {
                m[0] = 2 * nz / (rt - l); m[5] = 2 * nz / (t - b); m[8] = (rt + l) / (rt - l); m[9] = (t + b) / (t - b);
                m[10] = -(fz + nz) / (fz - nz); m[11] = -1; m[14] = -2 * fz * nz / (fz - nz);
            } else {
                m[0] = 2 / (rt - l); m[5] = 2 / (t - b); m[10] = -2 / (fz - nz);
                m[12] = -(rt + l) / (rt - l); m[13] = -(t + b) / (t - b); m[14] = -(fz + nz) / (fz - nz); m[15] = 1;
            }
            Mul(Stack().back().m, m);
            return 0;
        }
        if (n == "glVertexPointer") { vertex_ = {vertex_.on, static_cast<int>(r[0]), r[1], r[2], r[3]}; return 0; }
        if (n == "glColorPointer") { color_ = {color_.on, static_cast<int>(r[0]), r[1], r[2], r[3]}; return 0; }
        if (n == "glNormalPointer") return 0;
        if (n == "glTexCoordPointer") { Array& t = texcoord_[client_unit_ & 1]; t = {t.on, static_cast<int>(r[0]), r[1], r[2], r[3]}; return 0; }
        if (n == "glEnableClientState" || n == "glDisableClientState") {
            const bool on = n == "glEnableClientState";
            if (r[0] == kVertexArray) vertex_.on = on;
            else if (r[0] == kColorArray) color_.on = on;
            else if (r[0] == kTexCoordArray) texcoord_[client_unit_ & 1].on = on;
            return 0;
        }
        if (n == "glClientActiveTexture") { client_unit_ = static_cast<int>(r[0] - kTexture0); return 0; }
        if (n == "glActiveTexture") { unit_ = static_cast<int>(r[0] - kTexture0) & 1; return 0; }
        if (n == "glEnable" || n == "glDisable") {
            const bool on = n == "glEnable";
            if (r[0] == kTexture2D) tex2d_[unit_] = on;
            else if (r[0] == kBlend) blend_ = on;
            else if (r[0] == kAlphaTest) alpha_test_ = on;
            return 0;
        }
        if (n == "glColor4x") { color4_[0] = Fx(r[0]); color4_[1] = Fx(r[1]); color4_[2] = Fx(r[2]); color4_[3] = Fx(r[3]); return 0; }
        if (n == "glGenTextures") { for (uint32_t i = 0; i < r[0]; ++i) { const uint32_t id = next_tex_++; uc_->MemWrite(r[1] + 4 * i, &id, 4); } return 0; }
        if (n == "glDeleteTextures") return 0;   // (kept: the models still use them)
        if (n == "glBindTexture") { bound_[unit_] = r[1]; return 0; }
        if (n == "glPixelStorei") { if (r[0] == kUnpackAlignment) unpack_ = (std::max<uint32_t>)(1u, r[1]); return 0; }
        if (n == "glTexImage2D") { TexImage(r); return 0; }
        if (n == "glCompressedTexImage2D") { CompressedTexImage(r); return 0; }
        if (n == "glDrawArrays") { Record(r[0], r[1], r[2], 0, 0); return 0; }
        if (n == "glDrawElements") { Record(r[0], 0, r[1], r[2] ? r[2] : kUShort, r[3]); return 0; }
        if (n == "glGetIntegerv") {
            uint32_t v = 0;
            if (r[0] == 0x84E2) v = 2;          // GL_MAX_TEXTURE_UNITS
            else if (r[0] == 0x0D33) v = 1024;  // GL_MAX_TEXTURE_SIZE
            if (r[1]) uc_->MemWrite(r[1], &v, 4);
            return 0;
        }
        return 0;   // other state, queries, eglSwapBuffers: nothing to record
    }

    void TexImage(const uint32_t r[4]) {   // target, level, ifmt, w, h, border, format, type, pixels
        const uint32_t level = r[1], w = r[3], h = Arg(r, 4), fmt = Arg(r, 6), type = Arg(r, 7), pix = Arg(r, 8);
        if (level != 0 || !w || !h || w > 4096 || h > 4096) return;
        const int bpp = (type == k4444 || type == k5551 || type == k565) ? 2 : fmt == kRGBA ? 4 : fmt == kRGB ? 3 : fmt == kLuminanceAlpha ? 2 : 1;
        const uint32_t row = (w * bpp + unpack_ - 1) / unpack_ * unpack_;
        std::vector<uint8_t> raw(static_cast<size_t>(row) * h);
        if (pix) uc_->MemRead(pix, raw.data(), raw.size());
        Tex& t = textures_[bound_[unit_]];
        t.w = static_cast<int>(w); t.h = static_cast<int>(h);
        t.rgba.assign(static_cast<size_t>(w) * h, 0);
        t.hash = InUpload() ? upload_hash_ : 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = &raw[static_cast<size_t>(y) * row + static_cast<size_t>(x) * bpp];
                uint32_t R = 0, G = 0, B = 0, A = 255;
                const uint16_t s = static_cast<uint16_t>(p[0] | (bpp > 1 ? p[1] << 8 : 0));
                if (type == k5551) { R = (s >> 11) * 255 / 31; G = ((s >> 6) & 31) * 255 / 31; B = ((s >> 1) & 31) * 255 / 31; A = (s & 1) ? 255 : 0; }
                else if (type == k4444) { R = (s >> 12) * 17; G = ((s >> 8) & 15) * 17; B = ((s >> 4) & 15) * 17; A = (s & 15) * 17; }
                else if (type == k565) { R = (s >> 11) * 255 / 31; G = ((s >> 5) & 63) * 255 / 63; B = (s & 31) * 255 / 31; }
                else if (fmt == kRGBA) { R = p[0]; G = p[1]; B = p[2]; A = p[3]; }
                else if (fmt == kRGB) { R = p[0]; G = p[1]; B = p[2]; }
                else if (fmt == kLuminanceAlpha) { R = G = B = p[0]; A = p[1]; }
                else if (fmt == kAlpha) { R = G = B = 255; A = p[0]; }
                else { R = G = B = p[0]; }
                t.rgba[static_cast<size_t>(y) * w + x] = R | (G << 8) | (B << 16) | (A << 24);
            }
    }

    void CompressedTexImage(const uint32_t r[4]) {   // target, level, ifmt, w, h, border, size, data
        const uint32_t fmt = r[2], w = r[3], h = Arg(r, 4), size = Arg(r, 6), data = Arg(r, 7);
        if (fmt < kPalette4RGB8 || fmt > kPalette8RGB5A1 || static_cast<int32_t>(r[1]) > 0 || !w || !h || w > 4096 || h > 4096) return;
        const int idx = static_cast<int>(fmt - kPalette4RGB8), kind = idx % 5;
        const bool four = idx < 5;
        const int entries = four ? 16 : 256, esz = kind == 0 ? 3 : kind == 1 ? 4 : 2;
        std::vector<uint8_t> raw(size);
        uc_->MemRead(data, raw.data(), size);
        uint32_t pal[256] = {};
        for (int i = 0; i < entries && static_cast<size_t>(i * esz + esz) <= raw.size(); ++i) {
            const uint8_t* e = &raw[static_cast<size_t>(i) * esz];
            const uint16_t s = static_cast<uint16_t>(e[0] | (esz > 1 ? e[1] << 8 : 0));
            uint32_t R, G, B, A = 255;
            switch (kind) {
                case 0: R = e[0]; G = e[1]; B = e[2]; break;
                case 1: R = e[0]; G = e[1]; B = e[2]; A = e[3]; break;
                case 2: R = (s >> 11) * 255 / 31; G = ((s >> 5) & 63) * 255 / 63; B = (s & 31) * 255 / 31; break;
                case 3: R = (s >> 12) * 17; G = ((s >> 8) & 15) * 17; B = ((s >> 4) & 15) * 17; A = (s & 15) * 17; break;
                default: R = (s >> 11) * 255 / 31; G = ((s >> 6) & 31) * 255 / 31; B = ((s >> 1) & 31) * 255 / 31; A = (s & 1) ? 255 : 0; break;
            }
            pal[i] = R | (G << 8) | (B << 16) | (A << 24);
        }
        Tex& t = textures_[bound_[unit_]];
        t.w = static_cast<int>(w); t.h = static_cast<int>(h);
        t.hash = InUpload() ? upload_hash_ : 0;
        t.rgba.resize(static_cast<size_t>(w) * h);
        const size_t off = static_cast<size_t>(entries) * esz;
        for (size_t i = 0; i < t.rgba.size(); ++i) {
            const size_t b = off + (four ? i / 2 : i);
            const uint8_t v = b < raw.size() ? raw[b] : 0;
            t.rgba[i] = pal[four ? ((i & 1) ? (v & 15) : (v >> 4)) : v];
        }
    }

    bool InUpload() {
        uint32_t lr = 0;
        uc_->RegRead(Cpu::REG_LR, &lr);
        return lr >= G(kTextureUpload) && lr < G(kTextureUploadEnd);
    }

    static int TypeSize(uint32_t t) { return (t == kByte || t == kUByte) ? 1 : (t == kShort || t == kUShort) ? 2 : 4; }

    std::vector<float> Gather(const Array& a, int comps, uint32_t lo, uint32_t hi, bool normalized) {
        const uint32_t elem = TypeSize(a.type) * a.size, stride = a.stride ? a.stride : elem, n = hi - lo + 1;
        std::vector<uint8_t> raw(static_cast<size_t>(n - 1) * stride + elem);
        uc_->MemRead(a.ptr + lo * stride, raw.data(), raw.size());
        std::vector<float> out(static_cast<size_t>(n) * comps, 0.0f);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* p = raw.data() + static_cast<size_t>(i) * stride;
            float* o = &out[static_cast<size_t>(i) * comps];
            for (int c = 0; c < (std::min)(a.size, comps); ++c) {
                switch (a.type) {
                    case kByte: o[c] = normalized ? static_cast<int8_t>(p[c]) / 127.0f : static_cast<int8_t>(p[c]); break;
                    case kUByte: o[c] = normalized ? p[c] / 255.0f : p[c]; break;
                    case kShort: { int16_t s; memcpy(&s, p + 2 * c, 2); o[c] = normalized ? s / 32767.0f : s; break; }
                    case kUShort: { uint16_t s; memcpy(&s, p + 2 * c, 2); o[c] = normalized ? s / 65535.0f : s; break; }
                    case kFixed: { int32_t x; memcpy(&x, p + 4 * c, 4); o[c] = x / 65536.0f; break; }
                    default: memcpy(&o[c], p + 4 * c, 4); break;
                }
            }
            if (comps == 4 && a.size == 3) o[3] = 1.0f;
        }
        return out;
    }

    void Record(uint32_t mode, uint32_t first, uint32_t count, uint32_t idx_type, uint32_t idx_ptr) {
        if (!vertex_.on || !count || (mode != kTriangles && mode != kTriangleStrip && mode != kTriangleFan)) return;
        ModelSandbox::Draw d;
        d.mode = mode;
        d.count = count;
        uint32_t lo = first, hi = first + count - 1;
        if (idx_ptr || idx_type) {
            const int isz = idx_type == kUByte ? 1 : 2;
            std::vector<uint8_t> raw(static_cast<size_t>(count) * isz);
            uc_->MemRead(idx_ptr, raw.data(), raw.size());
            d.indices.resize(count);
            lo = 0xFFFFFFFF; hi = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t v = isz == 1 ? raw[i] : (raw[2 * i] | (raw[2 * i + 1] << 8));
                d.indices[i] = static_cast<uint16_t>(v);
                lo = (std::min)(lo, v); hi = (std::max)(hi, v);
            }
            for (auto& v : d.indices) v = static_cast<uint16_t>(v - lo);
        }
        if (hi - lo > 65535) return;
        std::vector<float> p = Gather(vertex_, vertex_.size, lo, hi, false);
        const uint32_t n = hi - lo + 1;
        d.pos.resize(static_cast<size_t>(n) * 3);
        for (uint32_t i = 0; i < n; ++i) {
            const float* v = &p[static_cast<size_t>(i) * vertex_.size];
            d.pos[i * 3] = v[0]; d.pos[i * 3 + 1] = v[1]; d.pos[i * 3 + 2] = vertex_.size > 2 ? v[2] : 0.0f;
        }
        if (color_.on) d.col = Gather(color_, 4, lo, hi, true);
        else memcpy(d.color, color4_, sizeof(color4_));
        const Array& tc = texcoord_[0];
        if (tc.on && tex2d_[0]) {
            std::vector<float> t = Gather(tc, tc.size, lo, hi, false);
            const float* tm = texm_.back().m;
            d.uv.resize(static_cast<size_t>(n) * 2);
            for (uint32_t i = 0; i < n; ++i) {
                const float s = t[static_cast<size_t>(i) * tc.size], u = tc.size > 1 ? t[static_cast<size_t>(i) * tc.size + 1] : 0.0f;
                const float q = tm[3] * s + tm[7] * u + tm[15], k = std::fabs(q) > 1e-12f ? 1.0f / q : 1.0f;
                d.uv[i * 2] = (tm[0] * s + tm[4] * u + tm[12]) * k;
                d.uv[i * 2 + 1] = (tm[1] * s + tm[5] * u + tm[13]) * k;
            }
            d.texture = static_cast<int>(bound_[0]);   // sandbox GL name for now; Load() maps it to an index
        }
        memcpy(d.mv, mv_.back().m, sizeof(d.mv));
        memcpy(d.proj, proj_.back().m, sizeof(d.proj));
        d.blend = blend_;
        d.alpha_test = alpha_test_;
        draws_.push_back(std::move(d));
    }

    std::unique_ptr<Cpu> cpu_;
    const std::vector<std::string>* imports_ = nullptr;
    const std::string empty_;
    uint32_t sp_ = 0, alloc_ = 0;
    std::map<uint32_t, uint32_t> sizes_;
    std::map<uint32_t, MemFile> mem_files_;
    std::set<uint32_t> disk_files_;
    uint32_t next_mem_ = 0x7000F000;
    uint32_t upload_hash_ = 0;
    // GL state
    uint32_t matrix_mode_ = kModelview;
    std::vector<Mat> mv_{1}, proj_{1}, texm_{1};
    Array vertex_, color_, texcoord_[2];
    int client_unit_ = 0, unit_ = 0;
    bool tex2d_[2] = {}, blend_ = false, alpha_test_ = false;
    float color4_[4] = {1, 1, 1, 1};
    uint32_t bound_[2] = {};
    uint32_t next_tex_ = 0x10000, unpack_ = 4;
};
}  // namespace

namespace {
// Names for resource hashes: every identifier-like string in the game's scripts and code, hashed like the
// resources (lower-case djb2). Animations are named in the scripts that play them ("snake_crawlForwards"); the
// ones only the code picks stay unnamed.
uint32_t HashName(const std::string& s) {
    uint32_t h = 5381;
    for (char c : s) h = h * 33 + static_cast<uint8_t>(tolower(static_cast<unsigned char>(c)));
    return h;
}

const std::map<uint32_t, std::string>& Names(Cpu* image) {
    static std::map<uint32_t, std::string> names;
    static bool built = false;
    if (built) return names;
    built = true;
    auto scan = [](const std::vector<uint8_t>& d) {
        std::string cur;
        for (size_t i = 0; i <= d.size(); ++i) {
            const char c = i < d.size() ? static_cast<char>(d[i]) : 0;
            if (isalnum(static_cast<unsigned char>(c)) || c == '_') { cur += c; continue; }
            if (cur.size() >= 3 && cur.size() <= 48) names.emplace(HashName(cur), cur);
            cur.clear();
        }
    };
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::u8path(VirtualFileSystem::Instance().AssetRoot());
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::string ext = it->path().extension().string();
        if (!it->is_regular_file() || (ext != ".lua" && ext != ".luc")) continue;
        std::ifstream f(it->path(), std::ios::binary);
        scan(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
    }
    std::vector<uint8_t> code(0x100000);
    if (image && image->MemRead(ImageBase(), code.data(), code.size()) == Cpu::OK) scan(code);
    // Names no string in the game spells out, found by hashing <family>_<words> combinations against the
    // animations' hashes (README.md §6.23; each one matches exactly, a few chance matches were left out).
    static const char* const kRecovered[] = {
    "box_crouchToIdle",
    "box_idleToCrouch",
    "box_sittingHoldShake",
    "chaff_aimInsert",
    "fists_squatPeekLt",
    "fists_squatPeekRt",
    "fists_wallKnock",
    "grenade_throw",
    "grenade_wallIdle",
    "nikita_aimIdle",
    "nikita_lookLtHold",
    "nikita_lookRtHold",
    "nikita_raise",
    "nikita_squatIdle",
    "nikita_squatPeekLt",
    "nikita_squatPeekRt",
    "nikita_wallIdle",
    "pistol_idle",
    "pistol_raise",
    "pistol_reload",
    "pistol_shoot",
    "pistol_squatPeekLt",
    "pistol_squatPeekRt",
    "pistol_wallKnock",
    "snake_crawlBackwards",
    "snake_crouchToCrawl",
    "snake_enterLocker",
    "snake_fallOver",
    "snake_famasShotC",
    "snake_getUp",
    "snake_grabGuard",
    "snake_idleToCrouch",
    "snake_idleToSquat",
    "snake_jumpOffBox",
    "snake_jumpOnBox",
    "snake_run",
    "snake_wallWalkLt",
    "snake_wallWalkRt",
    "soldier_deadBox",
    "soldier_deadDown",
    "soldier_deadUp",
    "soldier_die",
    "soldier_getUp",
    "soldier_idle",
    "soldier_idleAim",
    "soldier_idleToRadio",
    "soldier_kick",
    "soldier_openLocker",
    "soldier_proneIdle",
    "soldier_radioIdle",
    "soldier_radioToIdle",
    "soldier_run",
    "soldier_shoot",
    "soldier_slowWalk",
    "soldier_walkCaution",
    };
    for (const char* n : kRecovered) names.emplace(HashName(n), n);
    return names;
}

// The copy kept after a load, so models can be posed and rendered again (animation playback).
struct Session {
    std::unique_ptr<Sandbox> sb;
    std::vector<uint32_t> models;      // guest CIwModel per Result::models entry
    std::vector<uint32_t> skins;       // its CIwAnimSkin (0: not skinned)
    std::vector<uint32_t> anims;       // guest CIwAnim per Result::anims entry
    std::map<uint32_t, uint32_t> players;   // skeleton -> CIwAnimPlayer
    std::map<uint32_t, int> tex_index;      // copy's GL texture name -> Result::textures index
    uint32_t view = 0, ident = 0;
};
std::unique_ptr<Session> g_session;

// Renders one model of the session with IwGx and records the GL draws. posed: the skeleton to skin with
// (the skin's own skeleton = bind pose).
bool RenderModel(Session& se, size_t i, uint32_t posed, ModelSandbox::Model& mo, std::string* error) {
    Sandbox& sb = *se.sb;
    const uint32_t model = se.models[i], fn = sb.Read32(sb.Read32(model) + kModelRenderSlot);
    sb.draws_.clear();
    // Skinned: as CAnimated::Render sets the IwAnim context around the model render (0x4A0B8906)
    const uint32_t skin = se.skins[i], ctx = sb.Read32(G(kAnimContext));
    const bool skinned = skin && posed && ctx;
    if (skinned) {
        const uint32_t c[4] = {posed, skin, 0, sb.Read32(G(kAnimFrame))};
        sb.uc_->MemWrite(ctx + 4, c, sizeof(c));
    }
    sb.Call(G(kSetViewMatrix) | 1, {se.view});
    sb.Call(G(kSetModelMatrix) | 1, {se.ident, 0});
    sb.Call(fn, {model, 0, 2});
    sb.Call(G(kFlush) | 1, {});
    if (skinned) { const uint32_t c[4] = {}; sb.uc_->MemWrite(ctx + 4, c, sizeof(c)); }
    if (!sb.failed_.empty()) { *error = sb.failed_; return false; }
    mo.hash = sb.Read32(model + 4);
    mo.skeleton = skin ? sb.Read32(skin + 0x18) : 0;
    mo.draws = std::move(sb.draws_);
    for (ModelSandbox::Draw& d : mo.draws)
        if (d.texture >= 0) { auto it = se.tex_index.find(static_cast<uint32_t>(d.texture)); d.texture = it == se.tex_index.end() ? -1 : it->second; }
    return true;
}

// One attempt in a fresh copy. hide_all: hide every group the game has loaded, not just this one.
bool Attempt(const std::string& group, bool hide_all, ModelSandbox::Result& out, std::unique_ptr<Session>& session) {
    using namespace ModelSandbox;
    out = Result();
    auto se = std::make_unique<Session>();
    se->sb = std::make_unique<Sandbox>();
    Sandbox& sb = *se->sb;
    if (!sb.Init(&out.error)) return false;
    // Load the group with the game's resource manager; it becomes the manager's current group.
    const uint32_t rm = sb.Read32(G(kResManager));
    const uint32_t before = rm ? sb.Read32(rm + 0x54) : 0;
    // If the game has this group loaded already, hide it (and its child groups) from the copy's manager
    // (CIwResManager: group array +0x44, count +0x48; CIwResGroup: parent +0x30, name inline at +0x38): its
    // resources would count as loaded, and the new group's lists would keep dead placeholders for them. The other
    // groups stay, so references into them (the boss's soldier textures and skeleton) still resolve.
    if (rm) {
        auto norm = [](std::string s) {
            for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (!s.empty() && s[0] == '/') s.erase(0, 1);
            if (s.size() > 4 && s.compare(s.size() - 4, 4, ".bin") == 0) s.resize(s.size() - 4);
            return s;
        };
        auto name_of = [&](uint32_t g) { char b[128] = {}; sb.uc_->MemRead(g + 0x38, b, sizeof b - 1); return norm(b); };
        const std::string want = norm(group);
        const uint32_t arr = sb.Read32(rm + 0x44), n = (std::min<uint32_t>)(sb.Read32(rm + 0x48), 256u);
        std::vector<uint32_t> keep;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t g = sb.Read32(arr + 4 * i);
            bool hide = hide_all;
            for (uint32_t p = g, depth = 0; p && depth < 8 && !hide; p = sb.Read32(p + 0x30), ++depth) hide = name_of(p) == want;
            if (!hide) keep.push_back(g);
        }
        if (keep.size() != n) {
            if (!keep.empty()) sb.uc_->MemWrite(arr, keep.data(), 4 * keep.size());
            const uint32_t count = static_cast<uint32_t>(keep.size());
            sb.uc_->MemWrite(rm + 0x48, &count, 4);
        }
    }
    sb.Call(G(kLoadGroup) | 1, {sb.PutString(group), 0});
    const uint32_t grp = rm ? sb.Read32(rm + 0x54) : 0;
    out.files = sb.files_;
    if (!sb.failed_.empty()) { out.error = sb.failed_; return false; }
    if (!grp || grp == before) { out.error = "the game didn't load " + group; return false; }
    sb.Call(G(kResolveGroup) | 1, {grp, 1});   // as the game does after loading a character or item group (0x4A0444D0)
    // A resource list of the group (CIwResList; its CIwManagedList at +0xC: items, count)
    auto list_of = [&](const char* cls) {
        std::vector<uint32_t> v;
        const uint32_t l = sb.Call(G(kGetListNamed) | 1, {grp, sb.PutString(cls), 1});
        if (!l) return v;
        const uint32_t items = sb.Read32(l + 0xC), n = sb.Read32(l + 0x10);
        for (uint32_t i = 0; i < n && i < 4096; ++i) v.push_back(sb.Read32(items + 4 * i));
        return v;
    };
    const std::vector<uint32_t> models = list_of("CIwModel"), skins = list_of("CIwAnimSkin"), anims = list_of("CIwAnim");
    // CIwAnimSkin: +0x14 its model, +0x18 its skeleton, +0x1C / +0x20 the skin sets (the skinning callback's input)
    auto skin_of = [&](uint32_t model) -> uint32_t {
        for (uint32_t skin : skins) if (sb.Read32(skin + 0x14) == model) return skin;
        return 0;
    };
    const bool trace = getenv("MGS_TRACE_CAPTURE") != nullptr;
    if (trace)
        std::cout << "[Viewer]   group 0x" << std::hex << grp << ": " << std::dec << models.size() << " models, " << skins.size()
                  << " skins, " << anims.size() << " animations" << std::endl;
    // Render each model on its own: identity model matrix, a view looking down +z from far back (IwGx view
    // matrix = the camera's rotation rows at 4096 = 1, then its position).
    se->ident = sb.Alloc(48);
    se->view = sb.Alloc(48);
    const int32_t m[12] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096, 0, 0, 0};
    int32_t v[12] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096, 0, 0, -20000};
    sb.uc_->MemWrite(se->ident, m, sizeof(m));
    sb.uc_->MemWrite(se->view, v, sizeof(v));
    for (uint32_t model : models) {
        if (!model) continue;
        const uint32_t vtable = sb.Read32(model), fn = vtable ? sb.Read32(vtable + kModelRenderSlot) : 0;
        if (trace) {
            char b[120];
            snprintf(b, sizeof b, "[Viewer]   model 0x%08x: hash %08x, vtable 0x%08x, render 0x%08x", model, sb.Read32(model + 4), vtable, fn);
            std::cout << b << std::endl;
        }
        if (fn < ImageBase() || fn >= ImageBase() + 0x100000) { out.error = "a model has no render function"; continue; }
        se->models.push_back(model);
        se->skins.push_back(skin_of(model));
    }
    // Textures are uploaded while loading: copy GL names -> indices, so draws can name them
    for (auto& [name, t] : sb.textures_) {
        se->tex_index[name] = static_cast<int>(out.textures.size());
        out.textures.push_back({t.hash, t.w, t.h, std::move(t.rgba)});
    }
    sb.textures_.clear();
    // A freshly loaded skeleton is in its bind pose: skin with the skin's own skeleton.
    for (size_t i = 0; i < se->models.size(); ++i) {
        Model mo;
        const uint32_t skel = se->skins[i] ? sb.Read32(se->skins[i] + 0x18) : 0;
        if (!RenderModel(*se, i, skel, mo, &out.error)) { se->models.resize(i); se->skins.resize(i); break; }   // (state unknown)
        out.models.push_back(std::move(mo));
    }
    // CIwAnim: +4 hash, +0x10 skeleton, +0x14 bone mask, +0x18 / +0x1C keyframes, +0x28 length, +0x2C the
    // single-bone (root) animation played alongside. Only the ones for this group's skinned models are listed.
    const auto& names = Names(S3ERuntime::Instance().GuestCpu());
    for (uint32_t a : anims) {
        Anim an;
        an.hash = sb.Read32(a + 4);
        an.skeleton = sb.Read32(a + 0x10);
        an.length = sb.Read32(a + 0x28);
        an.keyframes = static_cast<int>(sb.Read32(a + 0x1C));
        bool used = false;
        for (const Model& mo : out.models) used = used || (mo.skeleton && mo.skeleton == an.skeleton);
        if (!used) continue;
        auto nm = names.find(an.hash);
        if (nm != names.end()) an.name = nm->second;
        if (trace) {
            char b[160];
            snprintf(b, sizeof b, "[Viewer]   anim #%zu %08x %s: %u (%.2f s), %d keyframes", out.anims.size() + 1, an.hash, an.name.c_str(), an.length,
                     an.length / 4096.0f, an.keyframes);
            std::cout << b << std::endl;
        }
        se->anims.push_back(a);
        out.anims.push_back(std::move(an));
    }
    if (VerboseLog())
        std::cout << "[Viewer] Loaded " << group << (hide_all ? " (other groups hidden)" : "") << ": " << models.size() << " models, "
                  << out.textures.size() << " textures, " << out.anims.size() << " animations, " << out.files.size() << " files"
                  << (out.error.empty() ? "" : ", " + out.error) << std::endl;
    if (sb.failed_.empty()) session = std::move(se);
    return out.error.empty() || !out.models.empty();
}
}  // namespace

bool ModelSandbox::Load(const std::string& group, Result& out) {
    g_session.reset();
    std::unique_ptr<Session> first;
    const bool ok = Attempt(group, false, out, first);
    if (ok && !out.models.empty() && out.error.empty()) { g_session = std::move(first); return true; }
    // Nothing, or dead entries: some of its resources count as loaded already under another group's name (the VR
    // pieces during gameplay). Again with every loaded group hidden; references into other groups won't resolve.
    Result alone;
    std::unique_ptr<Session> second;
    if (Attempt(group, true, alone, second) && alone.models.size() > out.models.size()) {
        out = std::move(alone);
        g_session = std::move(second);
        return true;
    }
    g_session = std::move(first);
    return ok;
}

bool ModelSandbox::Pose(size_t model, int anim, uint32_t time, Model& out, std::string* error) {
    if (!g_session || model >= g_session->models.size()) { *error = "nothing loaded"; return false; }
    Session& se = *g_session;
    Sandbox& sb = *se.sb;
    const uint32_t skin = se.skins[model];
    if (!skin) return RenderModel(se, model, 0, out, error);
    const uint32_t skel = sb.Read32(skin + 0x18);
    if (anim < 0 || anim >= static_cast<int>(se.anims.size())) return RenderModel(se, model, skel, out, error);   // bind pose
    // The game's animation player (CIwAnimPlayer, as CAnimated keeps one at +0xC4): ctor 0x4A075C30, SetSkel
    // 0x4A075A90 (keeps a posed copy of the skeleton at +0x4A4), PlayAnim 0x4A075AC0(player, anim, speed 1.0 =
    // 4096, flags, blend), Update 0x4A075A0C(player, dt): advance 0x4A0758C8, pose 0x4A0755B4, then 0x4A0757E8.
    uint32_t& player = se.players[skel];
    if (!player) {
        player = sb.Alloc(0x800);
        sb.Call(G(0x4A075C30) | 1, {player});
        sb.Call(G(0x4A075A90) | 1, {player, skel});
    }
    sb.Call(G(0x4A075AC0) | 1, {player, se.anims[anim], 0x1000, 0x100, 0});   // 0x100: restart even if it is playing
    sb.Call(G(0x4A075A0C) | 1, {player, time});
    const uint32_t posed = sb.Read32(player + 0x4A4);
    if (!sb.failed_.empty()) { *error = sb.failed_; g_session.reset(); return false; }
    return RenderModel(se, model, posed ? posed : skel, out, error);
}

void ModelSandbox::Release() { g_session.reset(); }
