#include "assetviewer.h"
#include "glheaders.h"
#include "gles.h"
#include "platform.h"
#include "settings.h"
#include "textures.h"
#include "modelsandbox.h"
#include "vfs.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {
constexpr GLenum kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41, kColorAttachment0 = 0x8CE0, kDepthAttachment = 0x8D00,
                 kFramebufferBinding = 0x8CA6, kReadFramebuffer = 0x8CA8, kReadFramebufferBinding = 0x8CAA,
                 kFramebufferComplete = 0x8CD5, kActiveTexture = 0x84E0, kTexture0 = 0x84C0;
#ifdef MGS_GLES
constexpr GLenum kDepthFormat = 0x81A5;   // GL_DEPTH_COMPONENT16_OES
#else
constexpr GLenum kDepthFormat = 0x81A6;   // GL_DEPTH_COMPONENT24
#endif

using PFN_Gen = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_Bind = void (APIENTRY*)(GLenum, GLuint);
using PFN_Storage = void (APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
using PFN_FbRenderbuffer = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
using PFN_FbTexture = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_Status = GLenum (APIENTRY*)(GLenum);
using PFN_Unit = void (APIENTRY*)(GLenum);

// Desktop: core name, then EXT. ES 1.1: the OES name only (as gles.cpp's LoadGL).
template <class T> T LoadGL(const char* core, const char* oes) {
#ifdef MGS_GLES
    (void)core;
    return reinterpret_cast<T>(GLES::Proc(oes));
#else
    (void)oes;
    void* p = GLES::Proc(core);
    if (!p) p = GLES::Proc((std::string(core) + "EXT").c_str());
    return reinterpret_cast<T>(p);
#endif
}

PFN_Unit ActiveTexture() {
    static PFN_Unit p = reinterpret_cast<PFN_Unit>(GLES::Proc("glActiveTexture"));
    return p;
}

// ---- textures ----
struct TexInfo {
    int w = 0, h = 0;
    std::string format;
    uint32_t hash = 0;     // CIwTexture hash, 0 = another upload
    std::string path;      // originals.txt path, "" if none
    bool replaced = false; // a custom PNG was uploaded instead
    std::string file;      // loaded from this group file by the Models tab (not one of the game's textures)
    uint32_t serial = 0;   // changes on every redefinition (captures check it before drawing with the name)
};
std::map<uint32_t, TexInfo> g_tex;   // GL name -> info
uint32_t g_tex_serial = 0;

std::string TexLabel(uint32_t name, const TexInfo& t) {
    char b[32];
    if (t.hash) snprintf(b, sizeof b, "%08x", t.hash);
    else snprintf(b, sizeof b, "GL texture %u", name);
    return b;
}

// The model a draw belongs to, from its texture's folder (the game's resource groups): "common\characters\snake\
// snake\f61114c5.png" -> "characters/snake", "common\items\socom\..." -> "items/socom". Every room material folder
// ("chapters\facility\mlist_12", "sharedtex") is the room itself.
std::string ModelName(bool textured, uint32_t hash) {
    if (!textured) return "(untextured: shadows, effects)";
    std::string p = hash ? TexturePack::OriginalPath(hash) : std::string();
    if (p.empty()) return "(other textures)";
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.rfind("chapters/", 0) == 0) return "Room geometry";
    p = p.substr(0, p.rfind('/'));   // the folder
    if (p.rfind("common/", 0) == 0) p = p.substr(7);
    const size_t last = p.rfind('/');   // "snake/snake" -> "snake"
    if (last != std::string::npos) {
        const size_t prev = p.rfind('/', last - 1);
        const std::string parent = p.substr(prev == std::string::npos ? 0 : prev + 1, last - (prev == std::string::npos ? 0 : prev + 1));
        if (parent == p.substr(last + 1)) p = p.substr(0, last);
    }
    return p;
}

// ---- frame capture ----
struct Mesh {
    std::vector<float> pos;        // xyz per vertex; world space once the capture is finished
    std::vector<float> uv;         // st per vertex (empty: no texture coordinates)
    std::vector<uint8_t> col;      // rgba per vertex (empty: `color` for all)
    std::vector<uint16_t> tris;    // triangle list
    std::vector<uint16_t> lines;   // wireframe edges, built on first use
    float color[4] = {1, 1, 1, 1};
    float mv[16] = {};
    bool eye = false;              // positions came in eye space (host-skinned)
    bool textured = false, blend = false, alpha_test = false;
    uint32_t tex = 0, tex_serial = 0, tex_hash = 0;
    uint32_t vptr = 0;
    std::string model;             // file loads: the CIwModel it belongs to (else grouped by texture folder)
};
struct Group {
    std::string label;
    std::vector<int> meshes;
    size_t tris = 0, verts = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
};

enum class Cap { Idle, Waiting, Recording };
Cap g_cap = Cap::Idle;
constexpr size_t kMaxDraws = 4096;
std::vector<Mesh> g_rec;       // the frame being recorded
std::vector<Mesh> g_meshes;    // the last finished capture
std::vector<Group> g_groups;   // [0] = the whole frame, then one per model (ModelName)
int g_capture_count = 0;
float g_world_up = 1.0f;       // +1: the game's camera up points to +y in its world, -1: to -y
std::string g_status;          // last export result
int g_pending_group = 0;       // selected when the capture finishes

bool TraceCapture() {
    static const bool on = getenv("MGS_TRACE_CAPTURE") != nullptr;
    return on;
}

// Column-major 4x4: out = a * b
void Mul(const float* a, const float* b, float* out) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
    memcpy(out, r, sizeof(r));
}

bool Invert(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

void Grow(float* lo, float* hi, const float* p) {
    for (int k = 0; k < 3; ++k) { lo[k] = (std::min)(lo[k], p[k]); hi[k] = (std::max)(hi[k], p[k]); }
}

// The recorded frame is complete: move every draw into the world space of the frame's main camera (the
// modelview most draws share: IwGx's world-space geometry, README §6.5), then group the draws into models.
void FinishCapture(bool quiet = false) {
    g_meshes.swap(g_rec);
    g_rec.clear();
    g_groups.clear();
    if (!quiet) ++g_capture_count;
    // The camera: the most common modelview (by triangle count).
    std::vector<std::pair<size_t, int>> votes;   // (triangles, first mesh with this matrix)
    for (int i = 0; i < static_cast<int>(g_meshes.size()); ++i) {
        const Mesh& m = g_meshes[i];
        if (m.eye) continue;
        bool found = false;
        for (auto& v : votes)
            if (!memcmp(g_meshes[v.second].mv, m.mv, sizeof(m.mv))) { v.first += m.tris.size(); found = true; break; }
        if (!found) votes.push_back({m.tris.size(), i});
    }
    float cam[16], cam_inv[16];
    for (int i = 0; i < 16; ++i) cam[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (!votes.empty()) memcpy(cam, g_meshes[std::max_element(votes.begin(), votes.end())->second].mv, sizeof(cam));
    if (!Invert(cam, cam_inv)) memcpy(cam_inv, cam, sizeof(cam));   // (identity)
    g_world_up = cam_inv[5] < 0.0f ? -1.0f : 1.0f;   // world direction of the camera's up axis: its y sign

    Group all;
    all.label = "Whole frame";
    std::map<std::string, int> by_model;
    for (int i = 0; i < static_cast<int>(g_meshes.size()); ++i) {
        Mesh& m = g_meshes[i];
        const bool world = !m.eye && !memcmp(m.mv, cam, sizeof(cam));   // already in world space
        float to_world[16];
        if (m.eye) memcpy(to_world, cam_inv, sizeof(to_world));
        else Mul(cam_inv, m.mv, to_world);
        if (!world)
            for (size_t v = 0; v + 2 < m.pos.size(); v += 3) {
                const float x = m.pos[v], y = m.pos[v + 1], z = m.pos[v + 2];
                const float* t = to_world;
                const float w = t[3] * x + t[7] * y + t[11] * z + t[15];
                const float s = std::fabs(w) > 1e-12f ? 1.0f / w : 1.0f;
                m.pos[v] = (t[0] * x + t[4] * y + t[8] * z + t[12]) * s;
                m.pos[v + 1] = (t[1] * x + t[5] * y + t[9] * z + t[13]) * s;
                m.pos[v + 2] = (t[2] * x + t[6] * y + t[10] * z + t[14]) * s;
            }
        const std::string model = m.model.empty() ? ModelName(m.textured, m.tex_hash) : m.model;
        auto it = by_model.find(model);
        if (it == by_model.end()) {
            it = by_model.emplace(model, static_cast<int>(g_groups.size())).first;
            g_groups.emplace_back();
            g_groups.back().label = model;
        }
        for (Group* g : {&all, &g_groups[it->second]}) {
            g->meshes.push_back(i);
            g->tris += m.tris.size() / 3;
            g->verts += m.pos.size() / 3;
            for (size_t v = 0; v + 2 < m.pos.size(); v += 3) Grow(g->lo, g->hi, &m.pos[v]);
        }
        if (TraceCapture()) {
            char b[200];
            snprintf(b, sizeof b, "[Capture] draw %d: vptr 0x%08x, %zu verts, %zu tris, tex %08x%s%s%s", i, m.vptr,
                     m.pos.size() / 3, m.tris.size() / 3, m.tex_hash, m.eye ? ", eye space" : "", m.blend ? ", blend" : "",
                     world ? "" : ", moved to world space");
            float avg[4] = {m.color[0], m.color[1], m.color[2], m.color[3]};   // mean vertex colour
            if (!m.col.empty()) {
                for (int k = 0; k < 4; ++k) avg[k] = 0.0f;
                for (size_t v = 0; v < m.col.size(); ++v) avg[v % 4] += m.col[v] / 255.0f;
                for (int k = 0; k < 4; ++k) avg[k] /= m.col.size() / 4;
            }
            std::cout << b << ", colour " << avg[0] << " " << avg[1] << " " << avg[2] << " " << avg[3] << " -> " << model << std::endl;
        }
    }
    std::stable_sort(g_groups.begin(), g_groups.end(), [](const Group& a, const Group& b) {
        return (a.label[0] == '(') != (b.label[0] == '(') ? b.label[0] == '(' : a.tris > b.tris;   // "(...)" last
    });
    g_groups.insert(g_groups.begin(), std::move(all));
    if (!quiet) std::cout << "[Viewer] Captured " << g_meshes.size() << " draws, " << g_groups[0].tris << " triangles, "
              << g_groups.size() - 1 << " models (world up " << (g_world_up > 0 ? "+y" : "-y") << ")" << std::endl;
}

// ---- preview (Models tab) ----
struct View {
    int group = 0;
    float yaw = 0.6f, pitch = 0.35f, dist = 1.0f, target[3] = {0, 0, 0}, radius = 1.0f;
    bool textured = true, vertex_colors = true, wireframe = false;
    bool want = false;   // the tab was drawn this frame: RenderPreview draws into the texture
    int w = 0, h = 0;    // preview size this frame (pixels)
} g_view;

struct Target {
    GLuint fbo = 0, tex = 0, depth = 0;
    int w = 0, h = 0;    // allocated size (powers of two)
    bool failed = false;
} g_rt;

// The orbit camera: eye position and its right / up / forward axes. "Up" is the game camera's up (g_world_up).
struct Basis { float eye[3], right[3], up[3], fwd[3]; };
void OrbitBasis(Basis& b) {
    const float u = g_world_up, cp = std::cos(g_view.pitch), sp = std::sin(g_view.pitch);
    const float dir[3] = {cp * std::sin(g_view.yaw), sp * u, cp * std::cos(g_view.yaw)};   // target -> eye
    for (int i = 0; i < 3; ++i) { b.eye[i] = g_view.target[i] + dir[i] * g_view.dist; b.fwd[i] = -dir[i]; }
    const float* f = b.fwd;
    b.right[0] = -f[2] * u; b.right[1] = 0.0f; b.right[2] = f[0] * u;   // fwd x (0, u, 0)
    const float l = std::sqrt(b.right[0] * b.right[0] + b.right[2] * b.right[2]);
    if (l > 1e-6f) { b.right[0] /= l; b.right[2] /= l; } else { b.right[0] = 1.0f; b.right[2] = 0.0f; }
    const float* r = b.right;
    b.up[0] = r[1] * f[2] - r[2] * f[1]; b.up[1] = r[2] * f[0] - r[0] * f[2]; b.up[2] = r[0] * f[1] - r[1] * f[0];
}

int Pot(int v) { int p = 64; while (p < v) p *= 2; return p; }

void ResetView() {
    if (g_view.group < 0 || g_view.group >= static_cast<int>(g_groups.size())) return;
    const Group& g = g_groups[g_view.group];
    // Frame the 1st..99th percentile per axis: a few stray vertices (particles far off) would shrink the rest.
    float r = 0.0f;
    std::vector<float> c;
    for (int k = 0; k < 3; ++k) {
        c.clear();
        for (int i : g.meshes) for (size_t v = k; v < g_meshes[i].pos.size(); v += 3) c.push_back(g_meshes[i].pos[v]);
        float lo = g.lo[k], hi = g.hi[k];
        if (c.size() >= 100) {
            std::nth_element(c.begin(), c.begin() + c.size() / 100, c.end());
            lo = c[c.size() / 100];
            std::nth_element(c.begin(), c.end() - 1 - c.size() / 100, c.end());
            hi = c[c.size() - 1 - c.size() / 100];
        }
        g_view.target[k] = (lo + hi) * 0.5f;
        r += (hi - lo) * (hi - lo);
    }
    g_view.radius = (std::max)(std::sqrt(r) * 0.5f, 1e-3f);
    g_view.dist = g_view.radius * 2.8f;
    g_view.yaw = 0.6f;
    g_view.pitch = 0.35f;
}

bool EnsureTarget(int w, int h) {
    if (g_rt.failed) return false;
    static PFN_Gen gen_fb = LoadGL<PFN_Gen>("glGenFramebuffers", "glGenFramebuffersOES");
    static PFN_Bind bind_fb = LoadGL<PFN_Bind>("glBindFramebuffer", "glBindFramebufferOES");
    static PFN_Gen gen_rb = LoadGL<PFN_Gen>("glGenRenderbuffers", "glGenRenderbuffersOES");
    static PFN_Bind bind_rb = LoadGL<PFN_Bind>("glBindRenderbuffer", "glBindRenderbufferOES");
    static PFN_Storage storage = LoadGL<PFN_Storage>("glRenderbufferStorage", "glRenderbufferStorageOES");
    static PFN_FbRenderbuffer fb_rb = LoadGL<PFN_FbRenderbuffer>("glFramebufferRenderbuffer", "glFramebufferRenderbufferOES");
    static PFN_FbTexture fb_tex = LoadGL<PFN_FbTexture>("glFramebufferTexture2D", "glFramebufferTexture2DOES");
    static PFN_Status status = LoadGL<PFN_Status>("glCheckFramebufferStatus", "glCheckFramebufferStatusOES");
    if (!gen_fb || !bind_fb || !gen_rb || !bind_rb || !storage || !fb_rb || !fb_tex) {
        std::cout << "[Viewer] No framebuffer objects: no model preview" << std::endl;
        g_rt.failed = true;
        return false;
    }
    bind_fb(kFramebuffer, g_rt.fbo);   // (callers restore the previous binding)
    if (w <= g_rt.w && h <= g_rt.h && g_rt.fbo) return true;
    const int tw = Pot((std::max)(w, g_rt.w)), th = Pot((std::max)(h, g_rt.h));
    if (!g_rt.fbo) { gen_fb(1, &g_rt.fbo); gen_rb(1, &g_rt.depth); glGenTextures(1, &g_rt.tex); }
    glBindTexture(GL_TEXTURE_2D, g_rt.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    bind_rb(kRenderbuffer, g_rt.depth);
    storage(kRenderbuffer, kDepthFormat, tw, th);
    bind_fb(kFramebuffer, g_rt.fbo);
    fb_tex(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_rt.tex, 0);
    fb_rb(kFramebuffer, kDepthAttachment, kRenderbuffer, g_rt.depth);
    if (status && status(kFramebuffer) != kFramebufferComplete) {
        std::cout << "[Viewer] Preview framebuffer incomplete: no model preview" << std::endl;
        g_rt.failed = true;
        return false;
    }
    g_rt.w = tw; g_rt.h = th;
    return true;
}

void BuildLines(Mesh& m) {
    std::set<std::pair<uint16_t, uint16_t>> edges;
    for (size_t i = 0; i + 2 < m.tris.size(); i += 3)
        for (int e = 0; e < 3; ++e) {
            uint16_t a = m.tris[i + e], b = m.tris[i + (e + 1) % 3];
            if (a > b) std::swap(a, b);
            edges.insert({a, b});
        }
    m.lines.clear();
    m.lines.reserve(edges.size() * 2);
    for (const auto& e : edges) { m.lines.push_back(e.first); m.lines.push_back(e.second); }
}

bool TextureAlive(const Mesh& m) {
    auto it = g_tex.find(m.tex);
    return it != g_tex.end() && it->second.serial == m.tex_serial;
}

void DrawMesh(const Mesh& m) {
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, m.pos.data());
    const bool tex = g_view.textured && m.textured && !m.uv.empty() && TextureAlive(m);
    if (tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, m.tex);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, m.uv.data());
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (g_view.vertex_colors && !m.col.empty()) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, m.col.data());
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        if (g_view.vertex_colors) glColor4f(m.color[0], m.color[1], m.color[2], m.color[3]);
        else glColor4f(1, 1, 1, 1);
    }
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m.tris.size()), GL_UNSIGNED_SHORT, m.tris.data());
}
}  // namespace

// ---- gles.cpp hooks ----
void AssetViewer::OnTexImage(uint32_t name, int w, int h, const char* format, uint32_t hash, bool replaced) {
    TexInfo& t = g_tex[name];
    t.w = w; t.h = h; t.format = format; t.hash = hash; t.replaced = replaced;
    t.path = hash ? TexturePack::OriginalPath(hash) : std::string();
    t.serial = ++g_tex_serial;
}

void AssetViewer::OnDeleteTexture(uint32_t name) { g_tex.erase(name); }

bool AssetViewer::Recording() { return g_cap == Cap::Recording; }
bool AssetViewer::CaptureWaiting() { return g_cap == Cap::Waiting; }

void AssetViewer::RequestCapture(int group) {
    if (g_cap != Cap::Idle) return;
    g_cap = Cap::Waiting;
    g_pending_group = (std::max)(0, group);
    g_status.clear();
}

void AssetViewer::OnPresentBegin() {
    if (g_cap != Cap::Recording) return;
    g_cap = Cap::Idle;
    FinishCapture();
    g_view.group = (std::min)(g_pending_group, static_cast<int>(g_groups.size()) - 1);
    g_pending_group = 0;
    ResetView();
}

void AssetViewer::OnPresentEnd() {
    if (g_cap != Cap::Waiting) return;
    g_cap = Cap::Recording;
    g_rec.clear();
}

namespace {
// The state a draw was made with (the live GL state, or recorded by the file loader).
struct MeshState {
    const float* mv = nullptr;       // null: positions are in eye space
    const float* texmat = nullptr;   // applied to the UVs; null: none
    bool textured = false, blend = false, alpha_test = false;
    uint32_t tex = 0;                // host GL texture
    float color[4] = {1, 1, 1, 1};   // when there is no colour array
};

// Triangles of one draw as a Mesh (false: nothing to show).
bool BuildMesh(const AssetViewer::DrawIn& d, const MeshState& st, Mesh& m) {
    m.eye = !st.mv;
    m.vptr = d.vptr;
    if (st.mv) memcpy(m.mv, st.mv, sizeof(m.mv));
    m.pos.resize(static_cast<size_t>(d.nverts) * 3);
    for (uint32_t i = 0; i < d.nverts; ++i) {
        const float* p = d.pos + static_cast<size_t>(i) * d.vsize;
        const float w = (d.vsize == 4 && p[3] != 0.0f) ? p[3] : 1.0f;
        m.pos[i * 3] = p[0] / w;
        m.pos[i * 3 + 1] = p[1] / w;
        m.pos[i * 3 + 2] = d.vsize > 2 ? p[2] / w : 0.0f;
    }
    if (d.uv) {
        m.uv.resize(static_cast<size_t>(d.nverts) * 2);
        const float* tm = st.texmat;   // IwGx scales its fixed-point UVs with the texture matrix
        for (uint32_t i = 0; i < d.nverts; ++i) {
            const float s = d.uv[static_cast<size_t>(i) * d.uvsize], t = d.uvsize > 1 ? d.uv[static_cast<size_t>(i) * d.uvsize + 1] : 0.0f;
            if (!tm) { m.uv[i * 2] = s; m.uv[i * 2 + 1] = t; continue; }
            const float q = tm[3] * s + tm[7] * t + tm[15], k = std::fabs(q) > 1e-12f ? 1.0f / q : 1.0f;
            m.uv[i * 2] = (tm[0] * s + tm[4] * t + tm[12]) * k;
            m.uv[i * 2 + 1] = (tm[1] * s + tm[5] * t + tm[13]) * k;
        }
    }
    if (d.col) {
        m.col.resize(static_cast<size_t>(d.nverts) * 4);
        for (size_t i = 0; i < m.col.size(); ++i)
            m.col[i] = static_cast<uint8_t>((std::max)(0.0f, (std::min)(1.0f, d.col[i])) * 255.0f + 0.5f);
    } else {
        memcpy(m.color, st.color, sizeof(m.color));
    }
    const bool indexed = d.indices && !d.indices->empty();
    auto at = [&](uint32_t i) -> uint16_t { return indexed ? (*d.indices)[i] : static_cast<uint16_t>(i); };
    auto tri = [&](uint16_t a, uint16_t b, uint16_t c) {
        if (a == b || b == c || a == c || a >= d.nverts || b >= d.nverts || c >= d.nverts) return;   // degenerate (strip joins)
        m.tris.push_back(a); m.tris.push_back(b); m.tris.push_back(c);
    };
    if (d.mode == GL_TRIANGLES) for (uint32_t i = 0; i + 2 < d.count; i += 3) tri(at(i), at(i + 1), at(i + 2));
    else if (d.mode == GL_TRIANGLE_STRIP) for (uint32_t i = 2; i < d.count; ++i) {
        if (i % 2 == 0) tri(at(i - 2), at(i - 1), at(i)); else tri(at(i - 1), at(i - 2), at(i));
    }
    else for (uint32_t i = 2; i < d.count; ++i) tri(at(0), at(i - 1), at(i));
    if (m.tris.empty()) return false;
    m.textured = st.textured;
    m.tex = st.tex;
    auto it = g_tex.find(m.tex);
    if (it != g_tex.end()) { m.tex_serial = it->second.serial; m.tex_hash = it->second.hash; }
    m.blend = st.blend;
    m.alpha_test = st.alpha_test;
    return true;
}
}  // namespace

void AssetViewer::AddDraw(const DrawIn& d) {
    if (g_cap != Cap::Recording || g_rec.size() >= kMaxDraws || d.nverts == 0) return;
    if (d.mode != GL_TRIANGLES && d.mode != GL_TRIANGLE_STRIP && d.mode != GL_TRIANGLE_FAN) return;
    float pr[16], mv[16], tm[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pr);
    if (pr[3] == 0.0f && pr[7] == 0.0f && pr[11] == 0.0f && pr[15] != 0.0f) return;   // 2D: HUD, fades, text
    MeshState st;
    if (!d.eye_space) { glGetFloatv(GL_MODELVIEW_MATRIX, mv); st.mv = mv; }
    if (!d.col) glGetFloatv(GL_CURRENT_COLOR, st.color);
    GLint active = kTexture0, bound = 0;   // texture unit 0 (the game uses one unit)
    glGetIntegerv(kActiveTexture, &active);
    if (ActiveTexture()) ActiveTexture()(kTexture0);
    st.textured = glIsEnabled(GL_TEXTURE_2D) != 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetFloatv(GL_TEXTURE_MATRIX, tm);
    st.texmat = tm;
    if (ActiveTexture()) ActiveTexture()(static_cast<GLenum>(active));
    st.tex = static_cast<uint32_t>(bound);
    st.blend = glIsEnabled(GL_BLEND) != 0;
    st.alpha_test = glIsEnabled(GL_ALPHA_TEST) != 0;
    Mesh m;
    if (BuildMesh(d, st, m)) g_rec.push_back(std::move(m));
}

// ---- Textures tab ----
namespace {
void DisableBlend(const ImDrawList*, const ImDrawCmd*) { glDisable(GL_BLEND); }

void Checkerboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float cell) {
    dl->AddRectFilled(p0, p1, IM_COL32(90, 90, 90, 255));
    const ImVec2 cmin = dl->GetClipRectMin(), cmax = dl->GetClipRectMax();   // only the visible cells
    const float x0 = (std::max)(p0.x, cmin.x), y0 = (std::max)(p0.y, cmin.y), x1 = (std::min)(p1.x, cmax.x), y1 = (std::min)(p1.y, cmax.y);
    for (float y = p0.y + std::floor((y0 - p0.y) / cell) * cell; y < y1; y += cell)
        for (float x = p0.x + std::floor((x0 - p0.x) / cell) * cell; x < x1; x += cell)
            if ((static_cast<int>((x - p0.x) / cell + 0.5f) + static_cast<int>((y - p0.y) / cell + 0.5f)) & 1)
                dl->AddRectFilled(ImVec2(x, y), ImVec2((std::min)(x + cell, p1.x), (std::min)(y + cell, p1.y)), IM_COL32(140, 140, 140, 255));
}

ImTextureRef Ref(GLuint tex) { return ImTextureRef(static_cast<ImTextureID>(tex)); }

bool Contains(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; };
    return lower(hay).find(lower(needle)) != std::string::npos;
}
}  // namespace

void AssetViewer::DrawTexturesTab() {
    static char filter[64] = "";
    static uint32_t selected = 0;
    static int zoom = 0;   // 0 = fit, else 2^(zoom-1)
    static bool opaque = false, in_capture = false;
    const float fs = ImGui::GetFontSize();

    std::unordered_set<uint32_t> captured;
    for (const Mesh& m : g_meshes) if (TextureAlive(m)) captured.insert(m.tex);
    std::vector<uint32_t> list;
    for (const auto& [name, t] : g_tex) {
        if (in_capture && !captured.count(name)) continue;
        if (!Contains(TexLabel(name, t) + " " + t.path, filter)) continue;
        list.push_back(name);
    }
    ImGui::Text("%d textures loaded", static_cast<int>(g_tex.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 12);
    ImGui::InputTextWithHint("##filter", "filter: hash or folder", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_meshes.empty());
    ImGui::Checkbox("In the captured frame", &in_capture);
    ImGui::EndDisabled();
    if (g_meshes.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Capture a frame in the Models tab first");

    const float list_w = ImGui::GetContentRegionAvail().x * 0.45f;
    if (ImGui::BeginChild("texlist", ImVec2(list_w, 0), ImGuiChildFlags_Borders)) {
        const float thumb = fs * 3.5f, pad = ImGui::GetStyle().FramePadding.x * 2, step = thumb + pad + ImGui::GetStyle().ItemSpacing.x;
        const int cols = (std::max)(1, static_cast<int>((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) / step));
        const int rows = (static_cast<int>(list.size()) + cols - 1) / cols;
        ImGuiListClipper clip;
        clip.Begin(rows, thumb + pad + ImGui::GetStyle().ItemSpacing.y);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
                for (int c = 0; c < cols; ++c) {
                    const int i = r * cols + c;
                    if (i >= static_cast<int>(list.size())) break;
                    const uint32_t name = list[i];
                    const TexInfo& t = g_tex[name];
                    if (c) ImGui::SameLine();
                    // Keep the aspect inside the square button
                    const float sx = t.w >= t.h ? 1.0f : static_cast<float>(t.w) / (std::max)(1, t.h);
                    const float sy = t.h >= t.w ? 1.0f : static_cast<float>(t.h) / (std::max)(1, t.w);
                    ImGui::PushID(static_cast<int>(name));
                    const bool sel = name == selected;
                    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::Button("##t", ImVec2(thumb + pad, thumb + pad))) selected = name;
                    if (sel) ImGui::PopStyleColor();
                    const ImVec2 c0(p.x + pad * 0.5f + thumb * (1 - sx) * 0.5f, p.y + pad * 0.5f + thumb * (1 - sy) * 0.5f);
                    const ImVec2 c1(c0.x + thumb * sx, c0.y + thumb * sy);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    Checkerboard(dl, c0, c1, thumb / 8);
                    dl->AddImage(Ref(name), c0, c1);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s  %dx%d\n%s", TexLabel(name, t).c_str(), t.w, t.h, t.path.empty() ? "(not a group texture)" : t.path.c_str());
                    ImGui::PopID();
                }
        if (list.empty()) ImGui::TextDisabled(g_tex.empty() ? "No textures yet." : "Nothing matches.");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("texinfo", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        auto it = g_tex.find(selected);
        if (it == g_tex.end()) {
            ImGui::TextDisabled("Pick a texture on the left.");
        } else {
            const TexInfo& t = it->second;
            ImGui::Text("%s   %d x %d   %s", TexLabel(selected, t).c_str(), t.w, t.h, t.format.c_str());
            if (!t.file.empty()) ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "From %s (Models tab)", t.file.c_str());
            if (!t.path.empty()) ImGui::TextWrapped("textures\\%s", t.path.c_str());
            else ImGui::TextDisabled("Not a group texture (font, software surface): not replaceable.");
            if (t.replaced) ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "Custom texture (replaced by a PNG)");
            else if (t.hash && TexturePack::HasReplacement(t.hash)) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "A custom PNG exists; it applies when the texture reloads");
            if (captured.count(selected)) ImGui::TextDisabled("Used in the captured frame");
            static const char* kZoom[] = { "Fit", "1x", "2x", "4x", "8x" };
            ImGui::SetNextItemWidth(fs * 5);
            ImGui::Combo("Zoom", &zoom, kZoom, 5);
            ImGui::SameLine();
            ImGui::Checkbox("Ignore alpha", &opaque);
            if (ImGui::BeginChild("texview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                float scale = zoom ? static_cast<float>(1 << (zoom - 1)) : (std::min)(avail.x / (std::max)(1, t.w), avail.y / (std::max)(1, t.h));
                if (!zoom) scale = (std::max)(scale, 0.05f);
                const ImVec2 size(t.w * scale, t.h * scale);
                const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
                ImGui::Dummy(size);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                Checkerboard(dl, p0, p1, (std::max)(fs * 0.5f, scale * 4));
                if (opaque) dl->AddCallback(DisableBlend, nullptr);
                dl->AddImage(Ref(selected), p0, p1);
                if (opaque) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
}

// ---- Models tab ----
namespace {
std::string SafeName(std::string s) {
    for (char& c : s) if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return s;
}

// The selected group as Wavefront OBJ + MTL in models\ next to the exe; materials point at the extracted PNGs.
std::string ExportObj(const Group& g) {
    std::error_code ec;
    const fs::path dir = Platform::ExeDir() / "models";
    fs::create_directories(dir, ec);
    char num[16];
    snprintf(num, sizeof num, "_%03d", g_capture_count);
    const std::string base = SafeName(g.label) + num;
    std::ofstream obj(dir / (base + ".obj")), mtl(dir / (base + ".mtl"));
    if (!obj || !mtl) return "Could not write to " + dir.string();
    obj << "# Metal Gear Solid Mobile, frame capture " << g_capture_count << ": " << g.label << " (" << g.tris << " triangles)\n";
    obj << "mtllib " << base << ".mtl\n";
    std::set<std::string> materials;
    size_t vbase = 1, tbase = 1;
    int n = 0;
    for (int i : g.meshes) {
        const Mesh& m = g_meshes[i];
        obj << "o draw_" << n++ << "\n";
        const size_t nv = m.pos.size() / 3;
        for (size_t v = 0; v < nv; ++v) {
            obj << "v " << m.pos[v * 3] << " " << m.pos[v * 3 + 1] << " " << m.pos[v * 3 + 2];
            if (!m.col.empty()) obj << " " << m.col[v * 4] / 255.0f << " " << m.col[v * 4 + 1] / 255.0f << " " << m.col[v * 4 + 2] / 255.0f;
            obj << "\n";
        }
        const bool uv = !m.uv.empty() && m.textured;
        if (uv) for (size_t v = 0; v < nv; ++v) obj << "vt " << m.uv[v * 2] << " " << 1.0f - m.uv[v * 2 + 1] << "\n";
        std::string mat = "untextured";
        if (m.textured && m.tex_hash) {
            char b[16]; snprintf(b, sizeof b, "tex_%08x", m.tex_hash);
            mat = b;
            if (materials.insert(mat).second) {
                std::string path = TexturePack::OriginalPath(m.tex_hash);
                std::replace(path.begin(), path.end(), '\\', '/');
                mtl << "newmtl " << mat << "\nKd 1 1 1\n";
                if (!path.empty()) mtl << "map_Kd ../textures/" << path << "\n";
                mtl << "\n";
            }
        } else if (materials.insert(mat).second) {
            mtl << "newmtl untextured\nKd 0.8 0.8 0.8\n\n";
        }
        obj << "usemtl " << mat << "\n";
        for (size_t t = 0; t + 2 < m.tris.size(); t += 3) {
            obj << "f";
            for (int k = 0; k < 3; ++k) {
                obj << " " << vbase + m.tris[t + k];
                if (uv) obj << "/" << tbase + m.tris[t + k];
            }
            obj << "\n";
        }
        vbase += nv;
        if (uv) tbase += nv;
    }
    return "Saved " + (dir / (base + ".obj")).string();
}
std::vector<std::string> g_files;   // every group under the asset root, as the game names them ("common/items/m9.group")
std::string g_loaded;               // the group shown, "" for a frame capture
std::vector<GLuint> g_file_textures;
std::vector<std::string> g_pending_loads;   // RequestLoad: one per frame

void ListFiles() {
    g_files.clear();
    std::error_code ec;
    const fs::path root = fs::u8path(VirtualFileSystem::Instance().AssetRoot());
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string rel = fs::relative(it->path(), root, ec).generic_u8string();
        const std::string ext = ".group.bin";
        if (!it->is_regular_file() || rel.size() <= ext.size() || rel.compare(rel.size() - ext.size(), ext.size(), ext) != 0) continue;
        // Only groups with models: their CIwModel resource list starts with the class hash (lower-case djb2 of
        // "CIwModel"). 77 of the 198 groups: the characters, the items and the mlist_N room pieces.
        std::ifstream f(it->path(), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        uint32_t h = 5381;
        for (char c : std::string("ciwmodel")) h = h * 33 + static_cast<uint8_t>(c);
        if (data.find(std::string(reinterpret_cast<const char*>(&h), 4)) == std::string::npos) continue;
        g_files.push_back(rel.substr(0, rel.size() - 4));   // the game adds ".bin"
    }
    std::sort(g_files.begin(), g_files.end());
    if (VerboseLog()) std::cout << "[Viewer] " << g_files.size() << " group files with models" << std::endl;
}

void FreeFileTextures() {
    for (GLuint t : g_file_textures) g_tex.erase(t);
    if (!g_file_textures.empty()) glDeleteTextures(static_cast<GLsizei>(g_file_textures.size()), g_file_textures.data());
    g_file_textures.clear();
}

ModelSandbox::Result g_file;          // the loaded group (models re-rendered by Pose; textures on the GPU)
std::vector<GLuint> g_file_host;      // Result::textures index -> host texture
std::vector<std::string> g_file_labels;
int g_anim = -1;                      // g_file.anims index shown, -1 = bind pose
float g_anim_time = 0.0f;             // seconds
float g_anim_speed = 1.0f;
bool g_anim_play = false;

// The loaded group's models as meshes (the capture's place), one entry per model.
void BuildFileMeshes(bool quiet) {
    g_rec.clear();
    for (size_t mi = 0; mi < g_file.models.size(); ++mi)
        for (const ModelSandbox::Draw& dr : g_file.models[mi].draws) {
            AssetViewer::DrawIn d;
            d.mode = dr.mode; d.indices = &dr.indices; d.count = dr.count;
            d.pos = dr.pos.data(); d.vsize = 3; d.nverts = static_cast<uint32_t>(dr.pos.size() / 3);
            if (!dr.uv.empty()) { d.uv = dr.uv.data(); d.uvsize = 2; }
            if (!dr.col.empty()) d.col = dr.col.data();
            MeshState st;
            st.mv = dr.mv;
            st.textured = dr.texture >= 0;
            st.tex = dr.texture >= 0 && dr.texture < static_cast<int>(g_file_host.size()) ? g_file_host[dr.texture] : 0;
            st.blend = dr.blend; st.alpha_test = dr.alpha_test;
            memcpy(st.color, dr.color, sizeof(st.color));
            Mesh m;
            if (BuildMesh(d, st, m)) { m.model = g_file_labels[mi]; g_rec.push_back(std::move(m)); }
        }
    FinishCapture(quiet);
    g_world_up = 1.0f;   // (the copy's camera says nothing about up; the game's world is y-up)
}

// The g_file model shown by a group entry (-1: none, or the whole frame)
int FileModelOf(int group) {
    if (g_loaded.empty() || group <= 0 || group >= static_cast<int>(g_groups.size())) return -1;
    for (size_t i = 0; i < g_file_labels.size(); ++i) if (g_file_labels[i] == g_groups[group].label) return static_cast<int>(i);
    return -1;
}

// Re-renders a model of the loaded group in the current animation pose (ModelSandbox::Pose).
void PoseFileModel(int mi) {
    if (mi < 0 || mi >= static_cast<int>(g_file.models.size())) return;
    const uint32_t t = static_cast<uint32_t>((std::max)(0.0f, g_anim_time) * 4096.0f);
    ModelSandbox::Model posed;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = ModelSandbox::Pose(static_cast<size_t>(mi), g_anim, t, posed, &err);
    if (TraceCapture()) {   // cost of a pose, averaged over 60
        static double total = 0; static int n = 0;
        total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (++n == 60) { std::cout << "[Viewer] Pose: " << total / n << " ms" << std::endl; total = 0; n = 0; }
    }
    if (!ok) {
        g_status = "Could not pose the model: " + err;
        g_anim_play = false;
        return;
    }
    g_file.models[mi] = std::move(posed);
    BuildFileMeshes(true);
}

// A group file through the game's own loader in a copy of the guest (ModelSandbox); its models replace the capture.
void LoadFile(const std::string& group) {
    const auto t0 = std::chrono::steady_clock::now();
    ModelSandbox::Result r;
    if (!ModelSandbox::Load(group, r)) {
        g_status = "Could not load " + group + ": " + r.error;
        if (group.rfind("chapters/facility/0", 0) == 0)   // the room groups set up the room itself (CRoom, PVS)
            g_status += ". Whole rooms don't load here yet; their pieces are in the mlist_N groups.";
        std::cout << "[Viewer] " << g_status << std::endl;
        return;
    }
    FreeFileTextures();
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    std::vector<GLuint> host(r.textures.size());
    for (size_t i = 0; i < r.textures.size(); ++i) {
        const ModelSandbox::Texture& t = r.textures[i];
        glGenTextures(1, &host[i]);
        glBindTexture(GL_TEXTURE_2D, host[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
        AssetViewer::OnTexImage(host[i], t.w, t.h, "RGBA8 (decoded)", t.hash, false);
        g_tex[host[i]].file = group;
        g_file_textures.push_back(host[i]);
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
    g_file_host = host;
    g_file_labels.clear();
    char label[96];
    for (size_t mi = 0; mi < r.models.size(); ++mi) {
        const ModelSandbox::Model& mo = r.models[mi];
        // Named after the texture it uses most, like the frame capture, plus its index and resource hash
        std::map<uint32_t, size_t> uses;
        for (const ModelSandbox::Draw& d : mo.draws)
            if (d.texture >= 0) uses[r.textures[d.texture].hash] += d.count;
        auto best = std::max_element(uses.begin(), uses.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        std::string name = best == uses.end() ? "untextured" : ModelName(true, best->first);
        if (name == "Room geometry") name = "room";
        snprintf(label, sizeof label, "%zu: %s (%08x)", mi + 1, name.c_str(), mo.hash);
        g_file_labels.push_back(label);
    }
    for (ModelSandbox::Texture& t : r.textures) std::vector<uint32_t>().swap(t.rgba);   // (on the GPU now)
    g_file = std::move(r);
    g_anim = -1;
    g_anim_time = 0.0f;
    g_anim_play = false;
    BuildFileMeshes(false);
    g_loaded = group;
    g_view.group = g_groups.size() > 1 ? 1 : 0;
    ResetView();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    char b[300];
    snprintf(b, sizeof b, "%s: %d models, %d textures, %d animations (%lld ms)%s%s", group.c_str(), static_cast<int>(g_file.models.size()),
             static_cast<int>(g_file.textures.size()), static_cast<int>(g_file.anims.size()), ms, g_file.error.empty() ? "" : ". Stopped early: ",
             g_file.error.c_str());
    g_status = b;
    if (g_file.models.empty())
        g_status += g_file.textures.empty() ? ". No models or textures in it." : ". No models in it; its textures are in the Textures tab.";
    std::cout << "[Viewer] " << g_status << std::endl;
}

// Animation picker and timeline for a skinned model of the loaded group: the game's own animation player poses it.
void DrawAnimationControls() {
    const int mi = FileModelOf(g_view.group);
    if (mi < 0 || !g_file.models[mi].skeleton) return;
    std::vector<int> list;   // the animations of this model's skeleton
    for (int i = 0; i < static_cast<int>(g_file.anims.size()); ++i)
        if (g_file.anims[i].skeleton == g_file.models[mi].skeleton) list.push_back(i);
    if (list.empty()) return;
    const float fs = ImGui::GetFontSize();
    auto label = [](int i) {
        if (i < 0) return std::string("Bind pose");
        const ModelSandbox::Anim& a = g_file.anims[i];
        char b[96];
        if (a.name.empty()) snprintf(b, sizeof b, "#%d  %08x  (%.2f s)", i + 1, a.hash, a.length / 4096.0f);
        else snprintf(b, sizeof b, "#%d  %s  (%.2f s)", i + 1, a.name.c_str(), a.length / 4096.0f);
        return std::string(b);
    };
    bool repose = false;
    static const char* test = getenv("MGS_VIEWER_ANIM");   // debug: "n[:seconds[:play]]", animation #n from there
    if (test) {
        int n = 0; float t = 0.0f;
        if (sscanf(test, "%d:%f", &n, &t) >= 1 && n >= 1 && n <= static_cast<int>(g_file.anims.size())) {
            g_anim = n - 1; g_anim_time = t; g_anim_play = strstr(test, ":play") != nullptr; repose = true;
        }
        test = nullptr;
    }
    bool reframe = false;
    ImGui::SetNextItemWidth(fs * 22);
    if (ImGui::BeginCombo("##anim", label(g_anim).c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Bind pose", g_anim < 0)) { g_anim = -1; repose = reframe = true; }
        for (int i : list)
            if (ImGui::Selectable(label(i).c_str(), g_anim == i)) { g_anim = i; g_anim_time = 0.0f; g_anim_play = true; repose = reframe = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d animations", static_cast<int>(list.size()));
    if (g_anim >= 0) {
        const float len = (std::max)(1.0f / 4096.0f, g_file.anims[g_anim].length / 4096.0f);
        if (ImGui::Button(g_anim_play ? "Pause" : "Play")) g_anim_play = !g_anim_play;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 14);
        if (ImGui::SliderFloat("##time", &g_anim_time, 0.0f, len, "%.2f s")) { g_anim_play = false; repose = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 6);
        ImGui::SliderFloat("Speed", &g_anim_speed, 0.1f, 2.0f, "%.1fx");
        if (g_anim_play) {
            g_anim_time += ImGui::GetIO().DeltaTime * g_anim_speed;
            if (g_anim_time >= len) g_anim_time = std::fmod(g_anim_time, len);   // loop
            repose = true;
        }
        // (just inside the end: the last instant is the next loop's start)
        g_anim_time = (std::min)(g_anim_time, len - 1.0f / 4096.0f);
    }
    if (repose) PoseFileModel(mi);
    if (reframe) ResetView();
}
}  // namespace

void AssetViewer::RequestLoad(const std::string& group) { g_pending_loads.push_back(group); }

void AssetViewer::DrawModelsTab() {
    const float fs = ImGui::GetFontSize();
    if (!g_pending_loads.empty()) { LoadFile(g_pending_loads.front()); g_pending_loads.erase(g_pending_loads.begin()); }
    ImGui::BeginDisabled(g_cap != Cap::Idle);
    if (ImGui::Button("Capture frame")) { g_loaded.clear(); g_file = ModelSandbox::Result(); ModelSandbox::Release(); RequestCapture(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Records the 3D draws of the next game frame (HUD and 2D left out).");
    ImGui::SameLine();
    // Any group file, loaded by the game's own code in a copy of the game (the game itself is left as it is)
    static char filter[64] = "";
    if (g_files.empty()) ListFiles();
    ImGui::SetNextItemWidth(fs * 20);
    if (ImGui::BeginCombo("##file", g_loaded.empty() ? "Load from the game files..." : g_loaded.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "filter (e.g. items, snake, mlist)", filter, sizeof filter);
        for (const std::string& f : g_files)
            if (Contains(f, filter) && ImGui::Selectable(f.c_str(), f == g_loaded)) LoadFile(f);
        ImGui::EndCombo();
    }
    if (g_cap != Cap::Idle) { ImGui::SameLine(); ImGui::TextDisabled("Capturing..."); }
    else if (g_meshes.empty() && g_loaded.empty()) { ImGui::SameLine(); ImGui::TextDisabled("Capture a frame, or load any model file."); }
    else if (g_loaded.empty() && !g_groups.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Capture %d: %d draws, %d triangles", g_capture_count, static_cast<int>(g_meshes.size()), static_cast<int>(g_groups[0].tris));
    }
    if (g_groups.empty()) { if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str()); return; }

    if (ImGui::BeginChild("models", ImVec2(fs * 15, 0), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(g_groups.size()); ++i) {
            const Group& g = g_groups[i];
            char label[160];
            snprintf(label, sizeof label, "%s##g%d", g.label.c_str(), i);
            if (i == 1) ImGui::Separator();
            if (ImGui::Selectable(label, g_view.group == i)) { g_view.group = i; ResetView(); }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%d draws, %d triangles, %d vertices", static_cast<int>(g.meshes.size()), static_cast<int>(g.tris), static_cast<int>(g.verts));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("preview", ImVec2(0, 0))) {
        ImGui::Checkbox("Textures", &g_view.textured);
        ImGui::SameLine();
        ImGui::Checkbox("Vertex colours", &g_view.vertex_colors);
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &g_view.wireframe);
        ImGui::SameLine();
        if (ImGui::Button("Reset view")) ResetView();
        ImGui::SameLine();
        if (ImGui::Button("Export OBJ") && g_view.group < static_cast<int>(g_groups.size())) {
            g_status = ExportObj(g_groups[g_view.group]);
            std::cout << "[Viewer] " << g_status << std::endl;
        }
        const Group& g = g_groups[(std::min)(g_view.group, static_cast<int>(g_groups.size()) - 1)];
        ImGui::TextDisabled("%s: %d draws, %d triangles.  Drag: rotate, right drag: pan, wheel: zoom",
                            g.label.c_str(), static_cast<int>(g.meshes.size()), static_cast<int>(g.tris));
        if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str());
        DrawAnimationControls();
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = (std::max)(size.x, 32.0f); size.y = (std::max)(size.y, 32.0f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActive()) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                g_view.yaw -= io.MouseDelta.x * 0.01f;
                g_view.pitch = (std::max)(-1.55f, (std::min)(1.55f, g_view.pitch + io.MouseDelta.y * 0.01f));
            } else {   // pan in the view plane
                Basis b;
                OrbitBasis(b);
                const float k = g_view.dist * 0.0025f;
                for (int i = 0; i < 3; ++i) g_view.target[i] += (-io.MouseDelta.x * b.right[i] + io.MouseDelta.y * b.up[i]) * k;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            if (io.MouseWheel != 0.0f) g_view.dist = (std::max)(g_view.radius * 0.05f, g_view.dist * std::pow(0.88f, io.MouseWheel));
        }
        g_view.w = static_cast<int>(size.x);
        g_view.h = static_cast<int>(size.y);
        g_view.want = true;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (g_rt.failed) {
            dl->AddText(p0, IM_COL32(255, 190, 80, 255), "No preview: framebuffer objects unavailable (Export OBJ still works).");
        } else if (g_rt.w > 0) {
            // The previous frame's render when the size just grew; RenderPreview redraws it before ImGui renders.
            const float u = static_cast<float>(g_view.w) / g_rt.w, v = static_cast<float>(g_view.h) / g_rt.h;
            dl->AddImage(Ref(g_rt.tex), p0, ImVec2(p0.x + size.x, p0.y + size.y), ImVec2(0, v), ImVec2(u, 0));
        }
    }
    ImGui::EndChild();
}

void AssetViewer::RenderPreview() {
    if (!g_view.want) return;
    g_view.want = false;
    if (g_groups.empty() || g_view.w <= 0 || g_view.h <= 0) return;
    GLint prev_fb = 0, prev_read = 0;
    glGetIntegerv(kFramebufferBinding, &prev_fb);
#ifndef MGS_GLES
    glGetIntegerv(kReadFramebufferBinding, &prev_read);
#endif
    static PFN_Bind bind_fb = LoadGL<PFN_Bind>("glBindFramebuffer", "glBindFramebufferOES");
    {
        GLStateSave save;
        if (!EnsureTarget(g_view.w, g_view.h)) {
            if (bind_fb) bind_fb(kFramebuffer, static_cast<GLuint>(prev_fb));
            return;
        }
        static PFN_Unit client = reinterpret_cast<PFN_Unit>(GLES::Proc("glClientActiveTexture"));
        if (ActiveTexture()) { ActiveTexture()(kTexture0 + 1); glDisable(GL_TEXTURE_2D); ActiveTexture()(kTexture0); }
        if (client) client(kTexture0);
        glViewport(0, 0, g_view.w, g_view.h);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.16f, 0.17f, 0.20f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glAlphaFunc(GL_GREATER, 0.1f);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glDisableClientState(GL_NORMAL_ARRAY);

        glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        const float near_z = (std::max)(g_view.dist - g_view.radius * 2.0f, g_view.dist * 0.01f), far_z = g_view.dist + g_view.radius * 2.0f;
        const float fy = near_z * std::tan(0.5f * 45.0f * 3.14159265f / 180.0f), fx = fy * g_view.w / g_view.h;
        glFrustum(-fx, fx, -fy, fy, near_z, far_z);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        {   // look at the target from the orbit position
            Basis b;
            OrbitBasis(b);
            const float* e = b.eye;
            const float m[16] = { b.right[0], b.up[0], -b.fwd[0], 0,  b.right[1], b.up[1], -b.fwd[1], 0,
                                  b.right[2], b.up[2], -b.fwd[2], 0,
                                  -(b.right[0] * e[0] + b.right[1] * e[1] + b.right[2] * e[2]),
                                  -(b.up[0] * e[0] + b.up[1] * e[1] + b.up[2] * e[2]),
                                  b.fwd[0] * e[0] + b.fwd[1] * e[1] + b.fwd[2] * e[2], 1 };
            glLoadMatrixf(m);
        }
        Group& g = g_groups[(std::min)(g_view.group, static_cast<int>(g_groups.size()) - 1)];
        if (g_view.wireframe) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(1.0f, 1.0f); }
        glEnable(GL_ALPHA_TEST);
        for (int i : g.meshes) if (!g_meshes[i].blend) DrawMesh(g_meshes[i]);   // opaque, then blended without depth writes
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (int i : g.meshes) if (g_meshes[i].blend) DrawMesh(g_meshes[i]);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        if (g_view.wireframe) {
            glDisable(GL_TEXTURE_2D);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glColor4f(0.35f, 1.0f, 0.5f, 1.0f);
            for (int i : g.meshes) {
                Mesh& m = g_meshes[i];
                if (m.lines.empty()) BuildLines(m);
                glVertexPointer(3, GL_FLOAT, 0, m.pos.data());
                glDrawElements(GL_LINES, static_cast<GLsizei>(m.lines.size()), GL_UNSIGNED_SHORT, m.lines.data());
            }
        }
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_TEXTURE); glPopMatrix();
    }
    bind_fb(kFramebuffer, static_cast<GLuint>(prev_fb));
#ifndef MGS_GLES
    bind_fb(kReadFramebuffer, static_cast<GLuint>(prev_read));
#endif
}
