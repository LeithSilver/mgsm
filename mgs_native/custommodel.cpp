#include "custommodel.h"
#include "loader.h"

#include "gles.h"
#include "glheaders.h"
#include "pngio.h"
#include "settings.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t kSkinCtx = 0x4A0EDBAC;   // IwAnim skinning context: +4 posed skeleton, +8 CIwAnimSkin
constexpr uint32_t kSnakeBones = 21, kSnakeVerts = 343;   // Snake's skeleton and skinned vertex count

struct Batch { int tex; std::vector<uint16_t> idx; };
struct Model {
    std::vector<std::vector<uint8_t>> pngs;
    std::vector<uint8_t> bone;
    std::vector<float> local;   // 3 per vertex, bone-local model units
    std::vector<float> uv;      // 2 per vertex
    std::vector<Batch> batches;
} g_model;
bool g_loaded = false;
bool g_folder = false;
std::string g_error = "not loaded";

std::vector<GLuint> g_tex;      // GL textures, made on the first draw
std::vector<float> g_posed;     // 3 per vertex, model space, from the last skinning
bool g_posed_ok = false;        // Snake has been skinned (and the model posed) with the option on
bool g_pending = false;         // posed since the last draw
std::unordered_set<uint32_t> g_snake_tex;   // GL names of Snake's textures

using PFN_ActiveTexture = void (APIENTRY*)(GLenum);
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif
constexpr GLenum GL_TEXTURE0_ = 0x84C0, GL_ACTIVE_TEXTURE_ = 0x84E0, GL_CLIENT_ACTIVE_TEXTURE_ = 0x84E1;

template <typename T> bool Get(const std::vector<uint8_t>& d, size_t& at, T& v) {
    if (at + sizeof(T) > d.size()) return false;
    memcpy(&v, &d[at], sizeof(T));
    at += sizeof(T);
    return true;
}

void MakeTextures() {
    for (const auto& png : g_model.pngs) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        std::vector<uint32_t> px;
        int w = 0, h = 0;
        if (PngIO::DecodeRGBA(png.data(), png.size(), px, w, h)) {
            glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        g_tex.push_back(t);
    }
}
}

bool CustomModel::Loaded() { return g_loaded; }
bool CustomModel::FolderPresent() { return g_folder; }
const std::string& CustomModel::Error() { return g_error; }
int CustomModel::TriangleCount() {
    size_t n = 0;
    for (const Batch& b : g_model.batches) n += b.idx.size() / 3;
    return static_cast<int>(n);
}

bool CustomModel::Load(const std::filesystem::path& path) {
    if (g_loaded) return true;
    std::error_code ec;
    g_folder = std::filesystem::is_directory(path.parent_path(), ec);
    std::ifstream f(path, std::ios::binary);
    if (!f) { g_error = "missing: " + path.string(); return false; }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Model m;
    size_t at = 4;
    uint32_t version = 0, n = 0;
    bool ok = d.size() > 8 && !memcmp(d.data(), "MGSM", 4) && Get(d, at, version) && version == 1 && Get(d, at, n);
    for (uint32_t i = 0; ok && i < n; ++i) {
        uint32_t size = 0;
        ok = Get(d, at, size) && at + size <= d.size();
        if (ok) { m.pngs.emplace_back(d.begin() + at, d.begin() + at + size); at += size; }
    }
    uint32_t nv = 0;
    ok = ok && Get(d, at, nv);
    for (uint32_t i = 0; ok && i < nv; ++i) {
        uint8_t bone = 0, pad = 0;
        int16_t p[3];
        uint16_t uv[2];
        ok = Get(d, at, bone) && Get(d, at, pad) && Get(d, at, p[0]) && Get(d, at, p[1]) && Get(d, at, p[2]) &&
             Get(d, at, uv[0]) && Get(d, at, uv[1]) && bone < kSnakeBones;
        m.bone.push_back(bone);
        for (int16_t c : p) m.local.push_back(c);
        m.uv.push_back(uv[0] / 65535.0f);
        m.uv.push_back(uv[1] / 65535.0f);
    }
    uint32_t nb = 0;
    ok = ok && Get(d, at, nb);
    for (uint32_t i = 0; ok && i < nb; ++i) {
        uint16_t tex = 0, pad = 0;
        uint32_t count = 0;
        ok = Get(d, at, tex) && Get(d, at, pad) && Get(d, at, count) && tex < m.pngs.size() && at + count * 2 <= d.size();
        if (!ok) break;
        Batch b{tex, std::vector<uint16_t>(count)};
        memcpy(b.idx.data(), &d[at], count * 2);
        at += count * 2;
        for (uint16_t v : b.idx) ok = ok && v < nv;
        m.batches.push_back(std::move(b));
    }
    if (!ok) { g_error = "not a valid model file: " + path.string(); return false; }
    g_model = std::move(m);
    g_posed.assign(g_model.local.size(), 0.0f);
    g_loaded = true;
    g_error.clear();
    std::cout << "[CustomModel] " << path.string() << ": " << nv << " vertices, " << TriangleCount() << " triangles, "
              << g_model.pngs.size() << " textures" << std::endl;
    return true;
}

void CustomModel::OnSkinned(Cpu* uc) {
    if (!g_loaded || !Settings::Instance().custom_player_model) { g_posed_ok = false; return; }
    auto rd = [&](uint32_t a) { uint32_t v = 0; uc->MemRead(a, &v, 4); return v; };
    uint32_t sp = 0;
    uc->RegRead(Cpu::REG_SP, &sp);
    const uint32_t count = rd(sp + 0x84), skel = rd(rd(G(kSkinCtx)) + 4);
    if (count != kSnakeVerts || rd(skel + 0x14) != kSnakeBones) return;   // not Snake
    // Each bone's matrix (bone + 0x24): 3x3 rows in 4.12, then the translation; v' = v . M / 4096 + t.
    float M[kSnakeBones][12];
    const uint32_t bones = rd(skel + 0x10);
    for (uint32_t b = 0; b < kSnakeBones; ++b) {
        int32_t m[12];
        if (uc->MemRead(rd(bones + b * 4) + 0x24, m, sizeof(m)) != Cpu::OK) return;
        for (int i = 0; i < 9; ++i) M[b][i] = m[i] / 4096.0f;
        for (int i = 9; i < 12; ++i) M[b][i] = static_cast<float>(m[i]);
    }
    for (size_t v = 0; v < g_model.bone.size(); ++v) {
        const float* m = M[g_model.bone[v]];
        const float* p = &g_model.local[v * 3];
        for (int c = 0; c < 3; ++c) g_posed[v * 3 + c] = p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + m[9 + c];
    }
    g_posed_ok = true;
    g_pending = true;
}

void CustomModel::NoteTexture(uint32_t name, const std::string& path) {
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    if (p.find("characters/snake/snake/") != std::string::npos) g_snake_tex.insert(name);
    else g_snake_tex.erase(name);
}

void CustomModel::ForgetTexture(uint32_t name) { g_snake_tex.erase(name); }

bool CustomModel::Covers() {
    if (!g_posed_ok || g_snake_tex.empty() || !Settings::Instance().custom_player_model) return false;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    return g_snake_tex.count(static_cast<uint32_t>(bound)) != 0;
}

void CustomModel::Draw(const float color[4]) {
    if (!g_pending) return;   // already drawn for this pose
    g_pending = false;
    static PFN_ActiveTexture p_active = reinterpret_cast<PFN_ActiveTexture>(GLES::Proc("glActiveTexture"));
    static PFN_ActiveTexture p_client = reinterpret_cast<PFN_ActiveTexture>(GLES::Proc("glClientActiveTexture"));
    GLint active = GL_TEXTURE0_, client = GL_TEXTURE0_, bound = 0, matrix_mode = GL_MODELVIEW, env = GL_MODULATE;
    glGetIntegerv(GL_ACTIVE_TEXTURE_, &active);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE_, &client);
    if (p_active) p_active(GL_TEXTURE0_);
    if (p_client) p_client(GL_TEXTURE0_);
    if (g_tex.empty()) {
        GLint keep = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &keep);
        MakeTextures();
        glBindTexture(GL_TEXTURE_2D, keep);
    }
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
    float current[4];
    glGetFloatv(GL_CURRENT_COLOR, current);
    const bool tex2d = glIsEnabled(GL_TEXTURE_2D), cull = glIsEnabled(GL_CULL_FACE), lighting = glIsEnabled(GL_LIGHTING);

    glMatrixMode(GL_TEXTURE);   // IwGx scales its fixed-point UVs here; ours are 0..1
    glPushMatrix();
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(color[0], color[1], color[2], 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, g_posed.data());
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, g_model.uv.data());
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    const bool linear = GLES::Instance().TextureFiltering();   // follows F9 like the game's textures
    for (const Batch& b : g_model.batches) {
        glBindTexture(GL_TEXTURE_2D, g_tex[b.tex]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(b.idx.size()), GL_UNSIGNED_SHORT, b.idx.data());
    }

    glPopMatrix();
    glMatrixMode(matrix_mode);
    glBindTexture(GL_TEXTURE_2D, bound);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
    glColor4f(current[0], current[1], current[2], current[3]);
    if (!tex2d) glDisable(GL_TEXTURE_2D);
    if (cull) glEnable(GL_CULL_FACE);
    if (lighting) glEnable(GL_LIGHTING);
    if (p_active) p_active(static_cast<GLenum>(active));
    if (p_client) p_client(static_cast<GLenum>(client));
}
