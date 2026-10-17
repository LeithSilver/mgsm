// OpenGL ES 1.1 (the subset gles.cpp, the overlay and the asset viewer use) on citro3d, for the 3DS port
// (README.md §6.28).
//
// Recording: GL state lives here (matrices, caps, textures, client arrays). Each draw converts its vertices to
// one float layout {pos xyzw, colour rgba, tc0 st, tc1 st} in a linear-memory arena and records a snapshot of
// the state it needs. Clears are recorded the same way.
// Replay (EndFrame, the GLES::Host swap): the frame is drawn into the top screen's left render target and, with
// the 3D slider up, again into the right one. Per eye, draws with a perspective projection get the eye's
// projection (shifted camera + frustum skew, converging at g_convergence); 2D draws (HUD, menus, text) stay
// at screen depth.
// Every projection also gets the 3DS conventions: the screen is rotated (the framebuffer is 240x400 portrait)
// and PICA clips z to [-w, 0] (GL: [-w, w]); glDepthRange becomes C3D_DepthMap.
//
// The GPU reads a frame's vertices while the next frame is recorded: two arenas. Textures are POT (resampled
// otherwise, at least 8x8), tiled, RGBA8 or the 16-bit format they came in.
// Lines and points become one-pixel quads, flat shading copies the provoking colour (the PICA has neither).
// Power-of-two textures keep the game's mip levels (sampled once uploaded in order).
// Not done: lighting and fog (the game sets them up but doesn't enable them in play: drawing with either on logs
// a warning), glReadPixels / glCopyTexImage2D (screenshots, the paused frame: no F1 menu on the 3DS yet).
#include "gl3ds.h"
#include "gl3ds_shbin.h"

#include <3ds.h>
#include <citro3d.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" bool g_profile;   // cpu_native.cpp: MGS_PROFILE=1 (per-call timing)

namespace {
constexpr int kScreenW = 400, kScreenH = 240;   // the top screen, landscape (GL coordinates: origin bottom left)
constexpr int kStackDepth = 32;
constexpr size_t kArenaBytes = 2 * 1024 * 1024;   // vertices per frame, x2 (double-buffered)
// Stereo convergence (the eye depth that appears at screen level) follows the scene: each frame's nearest 3D
// draw (by the depth of its vertices' centre), eased over a few frames. Close-up cameras then don't push the
// action far out of the screen. Starts at the gameplay camera's distance (~610).
float g_convergence = 600.0f;          // (game thread)
float g_replay_convergence = 600.0f;   // the frame being replayed (render thread)
float g_frame_near = 1e30f;
u64 g_record_ticks = 0, g_wait_ticks = 0, g_end_ticks = 0;   // timing (TakeStats)

struct Vertex { float pos[4], col[4], tc0[2], tc1[2]; };

struct ClientArray { bool on = false; int size = 4; GLenum type = GL_FLOAT; int stride = 0; const void* ptr = nullptr; };

// ---- GL state ------------------------------------------------------------------------------------------------
struct Mat { float m[16]; };
inline Mat Identity() { Mat r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
inline Mat Mul(const Mat& a, const Mat& b) {   // column-major a * b
    Mat r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r.m[c * 4 + row] = a.m[row] * b.m[c * 4] + a.m[4 + row] * b.m[c * 4 + 1] + a.m[8 + row] * b.m[c * 4 + 2] + a.m[12 + row] * b.m[c * 4 + 3];
    return r;
}

struct Stack {
    std::vector<Mat> s{Identity()};
    Mat& top() { return s.back(); }
};

struct TexUnit { bool enabled = false; GLuint bound = 0; GLint env_mode = GL_MODULATE; Stack matrix; };

struct Texture {
    C3D_Tex tex{};
    bool ready = false;
    int w = 0, h = 0;          // as the game gave it
    GLint min = GL_NEAREST_MIPMAP_LINEAR, mag = GL_LINEAR, wrap_s = GL_REPEAT, wrap_t = GL_REPEAT;
    std::vector<uint32_t> shadow;   // RGBA8 copy for resampled (NPOT) textures: glTexSubImage2D redoes them
    int mip_alloc = 0;   // mip levels allocated below level 0 (power-of-two textures)
    int mip_done = 0;    // levels 1..mip_done uploaded by the game (only those are sampled)
};

struct State {
    Stack mv, proj;
    GLenum matrix_mode = GL_MODELVIEW;
    TexUnit unit[2];
    int active = 0, client = 0;
    ClientArray vertex, color, normal, texcoord[2];
    float color4[4] = {1, 1, 1, 1};
    float clear_color[4] = {0, 0, 0, 0};
    float clear_depth = 1;
    int viewport[4] = {0, 0, kScreenW, kScreenH}, scissor[4] = {0, 0, kScreenW, kScreenH};
    bool scissor_on = false, depth_on = false, depth_mask = true, blend_on = false, alpha_on = false, cull_on = false;
    bool fog_on = false, lighting_on = false, color_material = false, dither = true;
    bool color_mask[4] = {true, true, true, true};
    GLenum depth_func = GL_LESS, alpha_func = GL_ALWAYS, blend_src = GL_ONE, blend_dst = GL_ZERO, cull_face = GL_BACK, front_face = GL_CCW;
    float alpha_ref = 0, depth_near = 0, depth_far = 1;
    GLenum shade = GL_SMOOTH;
    int unpack_align = 4, pack_align = 4;
    GLuint next_name = 1;
    GLuint fbo_bound = 0, rb_bound = 0;
    GLenum error = GL_NO_ERROR;
} S;

std::unordered_map<GLuint, Texture> g_textures;

// ---- recorded frame -------------------------------------------------------------------------------------------
struct Snapshot {
    Mat proj, mv;
    int viewport[4], scissor[4];
    bool scissor_on, depth_on, depth_mask, blend_on, alpha_on, cull_on, color_mask[4];
    GLenum depth_func, alpha_func, blend_src, blend_dst, cull_face, front_face;
    float alpha_ref, depth_near, depth_far;
    bool tex_on[2];
    GLuint tex[2];
    GLint env_mode[2];
    // The bound textures' GPU descriptors as of this draw: the render thread binds these and never looks at the
    // GL texture table, which the game's thread changes meanwhile.
    C3D_Tex texcopy[2];
};
enum class Kind { Draw, Clear };
struct Cmd {
    Kind kind;
    Snapshot st;
    GPU_Primitive_t prim;
    uint32_t first, count;   // vertices in the arena
    GLbitfield clear_mask;
    float clear_color[4];
};

struct Arena { Vertex* base = nullptr; size_t used = 0; };
// Three arenas: frame k records into arena k % 3 while the render thread replays frame k - 1 and the GPU may still
// read frame k - 2 (the render thread's C3D_FrameBegin for k - 1 waited for k - 2).
constexpr int kArenas = 3;
Arena g_arena[kArenas];
int g_cur = 0;
std::vector<Cmd> g_cmds;          // the top screen's draws this frame
std::vector<Cmd> g_cmds_bottom;   // the bottom screen's (the F1 overlay, between BeginBottom / EndBottom)
bool g_rec_bottom = false;        // recording into g_cmds_bottom
bool g_bottom_on = false;         // the bottom screen shows our render target (else the log console)
std::vector<Cmd>& Cmds() { return g_rec_bottom ? g_cmds_bottom : g_cmds; }
std::vector<C3D_Tex> g_deferred_free[kArenas];   // textures deleted while recording frame k (freed at k + 2)

bool g_ready = false;
C3D_RenderTarget* g_target[2] = {nullptr, nullptr};
bool g_stereo = true;   // SetStereo(false) (Old 3DS / 2DS): no right-eye target, the slider is ignored
C3D_RenderTarget* g_target_bottom = nullptr;
constexpr int kBottomW = 320;   // the bottom screen, landscape (same height as the top)
int g_replay_w = kScreenW;      // width of the screen being replayed into (render thread)
DVLB_s* g_dvlb = nullptr;
shaderProgram_s g_program;
int g_loc_proj = -1, g_loc_mv = -1;
int g_stereo_percent = 100;
bool g_warned_light = false, g_warned_fog = false, g_warned_arena = false;

void Warn(bool& once, const char* text) {
    if (once) return;
    once = true;
    std::cout << "[GL3DS] " << text << std::endl;
}

bool IsPerspective(const Mat& p) { return !(p.m[3] == 0 && p.m[7] == 0 && p.m[11] == 0 && p.m[15] != 0); }

Stack& CurrentStack() {
    switch (S.matrix_mode) {
        case GL_PROJECTION: return S.proj;
        case GL_TEXTURE: return S.unit[S.active].matrix;
        default: return S.mv;
    }
}

int TypeSize(GLenum t) {
    switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; default: return 4; }
}

Snapshot TakeSnapshot() {
    Snapshot s;
    s.proj = S.proj.top(); s.mv = S.mv.top();
    memcpy(s.viewport, S.viewport, sizeof(s.viewport)); memcpy(s.scissor, S.scissor, sizeof(s.scissor));
    s.scissor_on = S.scissor_on; s.depth_on = S.depth_on; s.depth_mask = S.depth_mask; s.blend_on = S.blend_on;
    s.alpha_on = S.alpha_on; s.cull_on = S.cull_on; memcpy(s.color_mask, S.color_mask, sizeof(s.color_mask));
    s.depth_func = S.depth_func; s.alpha_func = S.alpha_func; s.blend_src = S.blend_src; s.blend_dst = S.blend_dst;
    s.cull_face = S.cull_face; s.front_face = S.front_face;
    s.alpha_ref = S.alpha_ref; s.depth_near = S.depth_near; s.depth_far = S.depth_far;
    for (int u = 0; u < 2; ++u) {
        s.tex_on[u] = S.unit[u].enabled && S.unit[u].bound; s.tex[u] = S.unit[u].bound; s.env_mode[u] = S.unit[u].env_mode;
        if (s.tex_on[u]) {
            auto it = g_textures.find(s.tex[u]);
            if (it == g_textures.end() || !it->second.ready) s.tex_on[u] = false;
            else s.texcopy[u] = it->second.tex;
        }
    }
    return s;
}

Vertex* Alloc(size_t n) {
    Arena& a = g_arena[g_cur];
    if (!a.base || (a.used + n) * sizeof(Vertex) > kArenaBytes) { Warn(g_warned_arena, "vertex arena full, draws dropped this frame"); return nullptr; }
    Vertex* v = a.base + a.used;
    a.used += n;
    return v;
}

// One draw's vertices in draw order (indices resolved), as the shader's layout, into out[0..count).
// Element converters, picked once per array per draw (the switch on the type used to run per component).
using ConvFn = void (*)(const uint8_t* p, float* out, int n);
template <typename T> void ConvRaw(const uint8_t* p, float* o, int n) {
    for (int k = 0; k < n; ++k) { T v; memcpy(&v, p + sizeof(T) * k, sizeof(T)); o[k] = static_cast<float>(v); }
}
template <typename T, int kMax> void ConvNorm(const uint8_t* p, float* o, int n) {
    for (int k = 0; k < n; ++k) { T v; memcpy(&v, p + sizeof(T) * k, sizeof(T)); o[k] = v * (1.0f / kMax); }
}
void ConvFixed(const uint8_t* p, float* o, int n) {
    for (int k = 0; k < n; ++k) { int32_t v; memcpy(&v, p + 4 * k, 4); o[k] = v * (1.0f / 65536.0f); }
}
void ConvFloat(const uint8_t* p, float* o, int n) { memcpy(o, p, 4 * n); }

struct Reader {
    const uint8_t* base = nullptr;
    uint32_t stride = 0;
    int n = 0;
    ConvFn fn = nullptr;
    Reader(const ClientArray& a, bool normalized, int want) {
        if (!a.on || !a.ptr) return;
        base = static_cast<const uint8_t*>(a.ptr);
        stride = a.stride ? a.stride : TypeSize(a.type) * a.size;
        n = std::min(a.size, want);
        switch (a.type) {
            case GL_BYTE:           fn = normalized ? ConvNorm<int8_t, 127> : ConvRaw<int8_t>; break;
            case GL_UNSIGNED_BYTE:  fn = normalized ? ConvNorm<uint8_t, 255> : ConvRaw<uint8_t>; break;
            case GL_SHORT:          fn = normalized ? ConvNorm<int16_t, 32767> : ConvRaw<int16_t>; break;
            case GL_UNSIGNED_SHORT: fn = normalized ? ConvNorm<uint16_t, 65535> : ConvRaw<uint16_t>; break;
            case GL_FIXED:          fn = ConvFixed; break;
            default:                fn = ConvFloat; break;
        }
    }
    void operator()(uint32_t i, float* out) const { fn(base + static_cast<size_t>(i) * stride, out, n); }
};

// One draw's vertices in draw order (indices resolved), as the shader's layout, into out[0..count).
void GatherInto(Vertex* out, uint32_t count, const std::vector<uint32_t>* indices, uint32_t first) {
    const Mat& tm = S.unit[0].matrix.top();
    static const Mat kIdentity = Identity();
    const bool tex_matrix = memcmp(&tm, &kIdentity, sizeof(Mat)) != 0;
    const Reader pos(S.vertex, false, 4), col(S.color, true, 4), tc0(S.texcoord[0], false, 2), tc1(S.texcoord[1], false, 2);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t vi = indices ? (*indices)[i] : first + i;
        Vertex& v = out[i];
        v.pos[0] = v.pos[1] = v.pos[2] = 0; v.pos[3] = 1;
        pos(vi, v.pos);
        if (col.fn) { v.col[3] = 1; col(vi, v.col); }
        else memcpy(v.col, S.color4, sizeof(v.col));
        v.tc0[0] = v.tc0[1] = v.tc1[0] = v.tc1[1] = 0;
        if (tc0.fn) tc0(vi, v.tc0);
        if (tc1.fn) tc1(vi, v.tc1);
        if (tex_matrix) {
            const float s0 = v.tc0[0], t0 = v.tc0[1];
            v.tc0[0] = tm.m[0] * s0 + tm.m[4] * t0 + tm.m[12];
            v.tc0[1] = tm.m[1] * s0 + tm.m[5] * t0 + tm.m[13];
        }
    }
}
std::vector<Vertex> Gather(uint32_t count, const std::vector<uint32_t>* indices, uint32_t first) {
    std::vector<Vertex> out(count);
    GatherInto(out.data(), count, indices, first);
    return out;
}

// Triangles as a list, each with its GL provoking vertex (the last) colour on all three: flat shading, which the
// PICA doesn't have.
std::vector<Vertex> FlatTriangles(GLenum mode, const std::vector<Vertex>& v) {
    std::vector<Vertex> out;
    auto tri = [&](size_t a, size_t b, size_t c) {
        for (size_t k : {a, b, c}) { out.push_back(v[k]); memcpy(out.back().col, v[c].col, sizeof(v[c].col)); }
    };
    const size_t n = v.size();
    if (mode == GL_TRIANGLES) for (size_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2);
    else if (mode == GL_TRIANGLE_STRIP) for (size_t i = 0; i + 2 < n; ++i) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); }
    else for (size_t i = 1; i + 1 < n; ++i) tri(0, i, i + 1);   // fan
    return out;
}

// Lines and points as screen-space quads one pixel wide (the screen is the game's own resolution), in clip
// coordinates: drawn with identity matrices, so never shifted for stereo.
std::vector<Vertex> LineQuads(GLenum mode, const std::vector<Vertex>& v) {
    const Mat pmv = Mul(S.proj.top(), S.mv.top());
    auto clip = [&](const Vertex& in) {
        Vertex o = in;
        for (int r = 0; r < 4; ++r)
            o.pos[r] = pmv.m[r] * in.pos[0] + pmv.m[4 + r] * in.pos[1] + pmv.m[8 + r] * in.pos[2] + pmv.m[12 + r] * in.pos[3];
        return o;
    };
    const float px = 2.0f / std::max(1, S.viewport[2]), py = 2.0f / std::max(1, S.viewport[3]);   // one pixel in NDC
    std::vector<Vertex> out;
    auto quad = [&](Vertex a, Vertex b, float nx, float ny) {   // a..b widened by (nx, ny) NDC each side
        Vertex q[4] = {a, a, b, b};
        const float s[4] = {1, -1, 1, -1};
        for (int k = 0; k < 4; ++k) { q[k].pos[0] += s[k] * nx * q[k].pos[3]; q[k].pos[1] += s[k] * ny * q[k].pos[3]; }
        for (int k : {0, 1, 2, 1, 3, 2}) out.push_back(q[k]);
    };
    auto segment = [&](const Vertex& va, const Vertex& vb) {
        Vertex a = clip(va), b = clip(vb);
        if (a.pos[3] <= 0 || b.pos[3] <= 0) return;
        float dx = (b.pos[0] / b.pos[3] - a.pos[0] / a.pos[3]) / px, dy = (b.pos[1] / b.pos[3] - a.pos[1] / a.pos[3]) / py;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) { dx = 1; dy = 0; } else { dx /= len; dy /= len; }
        quad(a, b, -dy * 0.5f * px, dx * 0.5f * py);
    };
    const size_t n = v.size();
    if (mode == GL_POINTS) {
        for (const Vertex& p : v) {
            Vertex a = clip(p), b = a;
            if (a.pos[3] <= 0) continue;
            a.pos[0] -= 0.5f * px * a.pos[3]; b.pos[0] += 0.5f * px * b.pos[3];
            quad(a, b, 0, 0.5f * py);
        }
    } else if (mode == GL_LINES) {
        for (size_t i = 0; i + 1 < n; i += 2) segment(v[i], v[i + 1]);
    } else {   // strip / loop
        for (size_t i = 0; i + 1 < n; ++i) segment(v[i], v[i + 1]);
        if (mode == GL_LINE_LOOP && n > 2) segment(v[n - 1], v[0]);
    }
    return out;
}

// A 3D draw's depth (its vertices' centre, eye space) for the stereo convergence.
void NoteDepth(const Vertex* v, size_t n) {
    const Mat& mv = S.mv.top();
    float sum = 0;
    for (size_t i = 0; i < n; ++i) sum += -(mv.m[2] * v[i].pos[0] + mv.m[6] * v[i].pos[1] + mv.m[10] * v[i].pos[2] + mv.m[14] * v[i].pos[3]);
    const float depth = sum / n;
    if (depth > 1.0f) g_frame_near = std::min(g_frame_near, depth);
}

void RecordDraw(GLenum mode, uint32_t count, const std::vector<uint32_t>* indices, uint32_t first);
void Record(GLenum mode, uint32_t count, const std::vector<uint32_t>* indices, uint32_t first) {
    if (!g_profile) { RecordDraw(mode, count, indices, first); return; }
    const u64 t0 = svcGetSystemTick();
    RecordDraw(mode, count, indices, first);
    g_record_ticks += svcGetSystemTick() - t0;
}
void RecordDraw(GLenum mode, uint32_t count, const std::vector<uint32_t>* indices, uint32_t first) {
    if (!g_ready || count == 0 || !S.vertex.on) return;
    if (S.lighting_on) Warn(g_warned_light, "lighting is on but not implemented (vertex colours used as they are)");
    if (S.fog_on) Warn(g_warned_fog, "fog is on but not implemented");
    const bool triangles = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
    if (triangles && !(S.shade == GL_FLAT && S.color.on)) {   // the common case: straight into the arena
        Vertex* out = Alloc(count);
        if (!out) return;
        GatherInto(out, count, indices, first);
        if (IsPerspective(S.proj.top())) NoteDepth(out, count);
        Cmd c;
        c.kind = Kind::Draw;
        c.st = TakeSnapshot();
        c.prim = mode == GL_TRIANGLES ? GPU_TRIANGLES : mode == GL_TRIANGLE_STRIP ? GPU_TRIANGLE_STRIP : GPU_TRIANGLE_FAN;
        c.first = static_cast<uint32_t>(out - g_arena[g_cur].base);
        c.count = count;
        Cmds().push_back(c);
        return;
    }
    std::vector<Vertex> v = Gather(count, indices, first);
    if (IsPerspective(S.proj.top()) && triangles) NoteDepth(v.data(), v.size());
    GPU_Primitive_t prim = GPU_TRIANGLES;
    Snapshot st = TakeSnapshot();
    switch (mode) {
        case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
            if (S.shade == GL_FLAT && S.color.on) v = FlatTriangles(mode, v);
            else prim = mode == GL_TRIANGLES ? GPU_TRIANGLES : mode == GL_TRIANGLE_STRIP ? GPU_TRIANGLE_STRIP : GPU_TRIANGLE_FAN;
            break;
        case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: case GL_POINTS:
            v = LineQuads(mode, v);
            st.proj = Identity(); st.mv = Identity();
            st.cull_on = false;
            break;
        default: return;
    }
    if (v.empty()) return;
    Vertex* out = Alloc(v.size());
    if (!out) return;
    memcpy(out, v.data(), v.size() * sizeof(Vertex));
    Cmd c;
    c.kind = Kind::Draw;
    c.st = st;
    c.prim = prim;
    c.first = static_cast<uint32_t>(out - g_arena[g_cur].base);
    c.count = static_cast<uint32_t>(v.size());
    Cmds().push_back(c);
}

// ---- textures -----------------------------------------------------------------------------------------------
int Pot(int v) { int p = 8; while (p < v && p < 1024) p <<= 1; return p; }

// Morton order within the PICA's 8x8 tiles. The PICA's rows run the other way from GL's (t = 0 is the last
// row in memory; checked on hardware), so GL texel (x, y) goes to (x, h-1-y).
inline uint32_t TiledOffset(int x, int y, int w, int h) {
    y = h - 1 - y;
    static const uint8_t kMorton[64] = {
        0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
        32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51, 54, 55, 40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63 };
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + kMorton[(y & 7) * 8 + (x & 7)];
}

// GL pixels -> RGBA8 (0xRRGGBBAA) for byte formats; 16-bit packed types keep their bits (same layout on PICA).
bool Is16(GLenum type) { return type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1; }
GPU_TEXCOLOR GpuFormat(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5: return GPU_RGB565;
        case GL_UNSIGNED_SHORT_4_4_4_4: return GPU_RGBA4;
        case GL_UNSIGNED_SHORT_5_5_5_1: return GPU_RGBA5551;
        default: return GPU_RGBA8;
    }
}
int Components(GLenum format) {
    switch (format) { case GL_ALPHA: case GL_LUMINANCE: return 1; case GL_LUMINANCE_ALPHA: return 2; case GL_RGB: return 3; default: return 4; }
}
// One row of the game's pixels -> texel values: RGBA8 (0xRRGGBBAA) for byte formats; 16-bit packed types keep
// their bits (same layout on PICA). The format switch is per row, not per texel.
void ConvertRow(const uint8_t* p, int w, GLenum format, GLenum type, uint32_t* out) {
    if (Is16(type)) { for (int x = 0; x < w; ++x) { uint16_t v; memcpy(&v, p + 2 * x, 2); out[x] = v; } return; }
    switch (format) {
        case GL_ALPHA: for (int x = 0; x < w; ++x) out[x] = 0xFFFFFF00u | p[x]; break;
        case GL_LUMINANCE: for (int x = 0; x < w; ++x) { const uint32_t l = p[x]; out[x] = (l << 24) | (l << 16) | (l << 8) | 0xFF; } break;
        case GL_LUMINANCE_ALPHA: for (int x = 0; x < w; ++x, p += 2) { const uint32_t l = p[0]; out[x] = (l << 24) | (l << 16) | (l << 8) | p[1]; } break;
        case GL_RGB: for (int x = 0; x < w; ++x, p += 3) out[x] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | 0xFF; break;
        case GL_BGRA_EXT: for (int x = 0; x < w; ++x, p += 4) out[x] = (p[2] << 24) | (p[1] << 16) | (p[0] << 8) | p[3]; break;
        default: for (int x = 0; x < w; ++x, p += 4) out[x] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; break;
    }
}
size_t RowBytes(int w, GLenum format, GLenum type) {
    const size_t bytes = static_cast<size_t>(w) * (Is16(type) ? 2 : Components(format));
    const size_t a = S.unpack_align;
    return (bytes + a - 1) / a * a;
}

// A 16-bit packed texel -> RGBA8 (0xRRGGBBAA).
uint32_t Expand16(uint32_t v, GLenum type) {
    auto x = [](uint32_t c, uint32_t max) { return c * 255 / max; };
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5: return x((v >> 11) & 31, 31) << 24 | x((v >> 5) & 63, 63) << 16 | x(v & 31, 31) << 8 | 0xFF;
        case GL_UNSIGNED_SHORT_4_4_4_4: return x((v >> 12) & 15, 15) << 24 | x((v >> 8) & 15, 15) << 16 | x((v >> 4) & 15, 15) << 8 | x(v & 15, 15);
        default: return x((v >> 11) & 31, 31) << 24 | x((v >> 6) & 31, 31) << 16 | x((v >> 1) & 31, 31) << 8 | ((v & 1) ? 0xFF : 0);   // 5_5_5_1
    }
}

void ApplyTexParams(Texture& t) {
    auto filt = [](GLint f) { return (f == GL_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_LINEAR) ? GPU_LINEAR : GPU_NEAREST; };
    auto wrap = [](GLint w) { return w == GL_REPEAT ? GPU_REPEAT : w == GL_MIRRORED_REPEAT_OES ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE; };
    if (!t.ready) return;
    C3D_TexSetFilter(&t.tex, filt(t.mag), filt(t.min));
    C3D_TexSetWrap(&t.tex, wrap(t.wrap_s), wrap(t.wrap_t));
    // Mipmapping: only with a mipmapped minification filter, and only over the levels the game has uploaded.
    const bool mipmapped = t.min == GL_NEAREST_MIPMAP_NEAREST || t.min == GL_LINEAR_MIPMAP_NEAREST ||
                           t.min == GL_NEAREST_MIPMAP_LINEAR || t.min == GL_LINEAR_MIPMAP_LINEAR;
    C3D_TexSetFilterMipmap(&t.tex, (t.min == GL_NEAREST_MIPMAP_LINEAR || t.min == GL_LINEAR_MIPMAP_LINEAR) ? GPU_LINEAR : GPU_NEAREST);
    t.tex.maxLevel = static_cast<u8>(mipmapped ? t.mip_done : 0);
}

// Writes [x0, x0 + w) x [y0, y0 + h) of the game's image into the tiled texture (resampled if its size differs).
// level > 0: a mip level of a power-of-two texture (its own size, written into its slot of the chain).
void Store(Texture& t, int x0, int y0, int w, int h, const uint8_t* src, GLenum format, GLenum type, int level = 0) {
    const int tw = t.tex.width, th = t.tex.height;
    const bool same = tw == t.w && th == t.h;
    const bool sixteen = t.tex.fmt != GPU_RGBA8;
    const size_t row = RowBytes(w, format, type);
    std::vector<uint32_t> line(static_cast<size_t>(w));
    if (same) {
        const int lw = tw >> level, lh = th >> level;
        u32 bytes = 0;
        void* dst = C3D_Tex2DGetImagePtr(&t.tex, level, &bytes);
        for (int y = 0; y < h && y0 + y < lh; ++y) {
            ConvertRow(src + y * row, w, format, type, line.data());
            for (int x = 0; x < w && x0 + x < lw; ++x) {
                const uint32_t o = TiledOffset(x0 + x, y0 + y, lw, lh);
                if (sixteen) static_cast<uint16_t*>(dst)[o] = static_cast<uint16_t>(line[x]);
                else static_cast<uint32_t*>(dst)[o] = line[x];
            }
        }
        GSPGPU_FlushDataCache(dst, bytes);
        return;
    } else {   // NPOT: keep an RGBA8 copy and resample it (nearest) into the POT texture
        if (t.shadow.size() != static_cast<size_t>(t.w) * t.h) t.shadow.assign(static_cast<size_t>(t.w) * t.h, 0);
        for (int y = 0; y < h; ++y) {
            ConvertRow(src + y * row, w, format, type, line.data());
            uint32_t* dst = &t.shadow[static_cast<size_t>(y0 + y) * t.w + x0];
            for (int x = 0; x < w; ++x) dst[x] = Is16(type) ? Expand16(line[x], type) : line[x];   // (resampled: always RGBA8)
        }
        std::vector<int> sx(tw);
        for (int x = 0; x < tw; ++x) sx[x] = x * t.w / tw;
        uint32_t* dst = static_cast<uint32_t*>(t.tex.data);
        for (int y = 0; y < th; ++y) {
            const uint32_t* srow = &t.shadow[static_cast<size_t>(y * t.h / th) * t.w];
            for (int x = 0; x < tw; ++x) dst[TiledOffset(x, y, tw, th)] = srow[sx[x]];
        }
    }
    C3D_TexFlush(&t.tex);
}

Texture* Bound() {
    auto it = g_textures.find(S.unit[S.active].bound);
    return it == g_textures.end() ? nullptr : &it->second;
}

void FreeTex(Texture& t) {
    if (t.ready) g_deferred_free[g_cur].push_back(t.tex);
    t.ready = false;
}

// ---- replay ---------------------------------------------------------------------------------------------------
GPU_TESTFUNC TestFunc(GLenum f) {
    switch (f) {
        case GL_NEVER: return GPU_NEVER; case GL_LESS: return GPU_LESS; case GL_EQUAL: return GPU_EQUAL;
        case GL_LEQUAL: return GPU_LEQUAL; case GL_GREATER: return GPU_GREATER; case GL_NOTEQUAL: return GPU_NOTEQUAL;
        case GL_GEQUAL: return GPU_GEQUAL; default: return GPU_ALWAYS;
    }
}
GPU_BLENDFACTOR BlendFactor(GLenum f) {
    switch (f) {
        case GL_ZERO: return GPU_ZERO; case GL_ONE: return GPU_ONE;
        case GL_SRC_COLOR: return GPU_SRC_COLOR; case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return GPU_DST_COLOR; case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return GPU_SRC_ALPHA; case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return GPU_DST_ALPHA; case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return GPU_SRC_ALPHA_SATURATE; default: return GPU_ONE;
    }
}


// GL projection -> what the shader multiplies by: the eye's stereo shift (perspective only), then PICA's z
// (z' = -(z + w) / 2: near 0, far -w) and the rotated screen (x' = y, y' = -x).
C3D_Mtx PicaProjection(const Mat& p, float eye) {
    Mat g = p;
    if (eye != 0 && IsPerspective(p)) {
        // Camera moved by e along x (P * T(-e)) and the frustum skewed so that depth g_convergence stays put:
        // NDC shift e * P00 * (1 / D - 1 / g_convergence) for a point at depth D.
        const float e = eye * g_replay_convergence * 0.025f / std::max(p.m[0], 0.01f);
        for (int r = 0; r < 4; ++r) g.m[12 + r] -= e * p.m[r];          // * T(-e): column 3 -= e * column 0
        const float k = e * p.m[0] / g_replay_convergence;
        for (int c = 0; c < 4; ++c) g.m[c * 4] += k * g.m[c * 4 + 3];   // row 0 += k * row 3
    }
    float rows[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) rows[r][c] = g.m[c * 4 + r];
    float out[4][4];
    for (int c = 0; c < 4; ++c) {
        out[0][c] = rows[1][c];                              // x' = y
        out[1][c] = -rows[0][c];                             // y' = -x
        out[2][c] = -0.5f * rows[2][c] - 0.5f * rows[3][c];  // z' = -(z + w) / 2
        out[3][c] = rows[3][c];
    }
    C3D_Mtx m;
    for (int r = 0; r < 4; ++r) { m.r[r].x = out[r][0]; m.r[r].y = out[r][1]; m.r[r].z = out[r][2]; m.r[r].w = out[r][3]; }
    return m;
}

C3D_Mtx ToC3D(const Mat& a) {
    C3D_Mtx m;
    for (int r = 0; r < 4; ++r) { m.r[r].x = a.m[r]; m.r[r].y = a.m[4 + r]; m.r[r].z = a.m[8 + r]; m.r[r].w = a.m[12 + r]; }
    return m;
}

// Landscape GL rectangle (origin bottom left) -> the portrait render target's (x along the screen's height).
void ToTarget(const int r[4], u32& x, u32& y, u32& w, u32& h) {
    const int sw = g_replay_w;
    const int vx = std::clamp(r[0], 0, sw), vy = std::clamp(r[1], 0, kScreenH);
    const int vw = std::clamp(r[2], 0, sw - vx), vh = std::clamp(r[3], 0, kScreenH - vy);
    x = vy; y = sw - vx - vw; w = vh; h = vw;
}

void ApplyTexEnv(const Snapshot& s) {
    int stage = 0;
    bool any = false;
    for (int u = 0; u < 2; ++u) {
        if (!s.tex_on[u]) continue;
        C3D_TexBind(u, const_cast<C3D_Tex*>(&s.texcopy[u]));
        C3D_TexEnv* env = C3D_GetTexEnv(stage);
        C3D_TexEnvInit(env);
        const GPU_TEVSRC prev = stage == 0 ? GPU_PRIMARY_COLOR : GPU_PREVIOUS, none = GPU_PRIMARY_COLOR;
        const GPU_TEVSRC tex = u == 0 ? GPU_TEXTURE0 : GPU_TEXTURE1;
        switch (s.env_mode[u]) {
            case GL_REPLACE:
                C3D_TexEnvSrc(env, C3D_Both, tex, none, none);
                C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
                break;
            case GL_DECAL:
                C3D_TexEnvSrc(env, C3D_RGB, tex, prev, tex);
                C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
                C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
                C3D_TexEnvSrc(env, C3D_Alpha, prev, none, none);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
                break;
            case GL_ADD:
                C3D_TexEnvSrc(env, C3D_RGB, tex, prev, none);
                C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
                C3D_TexEnvSrc(env, C3D_Alpha, tex, prev, none);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
                break;
            default:   // GL_MODULATE (and GL_COMBINE, approximated)
                C3D_TexEnvSrc(env, C3D_Both, tex, prev, none);
                C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
                break;
        }
        ++stage;
        any = true;
    }
    if (!any) {
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        stage = 1;
    }
    for (int i = stage; i < 2; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));   // passthrough
}

void ApplyState(const Snapshot& s, float eye) {
    const C3D_Mtx proj = PicaProjection(s.proj, eye), mv = ToC3D(s.mv);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, &proj);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_mv, &mv);
    u32 x, y, w, h;
    ToTarget(s.viewport, x, y, w, h);
    C3D_SetViewport(x, y, std::max<u32>(w, 1), std::max<u32>(h, 1));
    if (s.scissor_on) {
        ToTarget(s.scissor, x, y, w, h);
        C3D_SetScissor(GPU_SCISSOR_NORMAL, x, y, x + w, y + h);
    } else C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    int mask = (s.color_mask[0] ? GPU_WRITE_RED : 0) | (s.color_mask[1] ? GPU_WRITE_GREEN : 0) |
               (s.color_mask[2] ? GPU_WRITE_BLUE : 0) | (s.color_mask[3] ? GPU_WRITE_ALPHA : 0);
    if (s.depth_on && s.depth_mask) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(true, s.depth_on ? TestFunc(s.depth_func) : GPU_ALWAYS, static_cast<GPU_WRITEMASK>(mask));
    C3D_DepthMap(true, -(s.depth_far - s.depth_near), s.depth_near);
    if (s.cull_on && s.cull_face != GL_FRONT_AND_BACK) {
        const bool back = s.cull_face == GL_BACK;
        const bool ccw = s.front_face == GL_CCW;
        C3D_CullFace(back == ccw ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW);
    } else C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(s.alpha_on, TestFunc(s.alpha_func), static_cast<int>(std::clamp(s.alpha_ref, 0.0f, 1.0f) * 255));
    if (s.blend_on)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, BlendFactor(s.blend_src), BlendFactor(s.blend_dst), BlendFactor(s.blend_src), BlendFactor(s.blend_dst));
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    ApplyTexEnv(s);
}

uint32_t ClearColorValue(const float c[4]) {
    auto b = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255 + 0.5f); };
    return (b(c[0]) << 24) | (b(c[1]) << 16) | (b(c[2]) << 8) | b(c[3]);
}

void Replay(const std::vector<Cmd>& cmds, const Arena& arena, float eye) {
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, arena.base, sizeof(Vertex), 4, 0x3210);
    for (size_t i = 0; i < cmds.size(); ++i) {
        const Cmd& c = cmds[i];
        if (c.kind == Kind::Clear && i == 0) continue;   // done by the render target clear
        ApplyState(c.st, eye);
        if (c.kind == Kind::Clear) {   // a later clear: a quad over the viewport (scissored), depth to far
            Snapshot s = c.st;
            s.depth_on = (c.clear_mask & GL_DEPTH_BUFFER_BIT) != 0;
            s.depth_func = GL_ALWAYS;
            s.depth_mask = s.depth_on;
            if (!(c.clear_mask & GL_COLOR_BUFFER_BIT)) s.color_mask[0] = s.color_mask[1] = s.color_mask[2] = s.color_mask[3] = false;
            s.blend_on = s.alpha_on = s.cull_on = false;
            s.tex_on[0] = s.tex_on[1] = false;
            s.proj = Identity(); s.mv = Identity();
            ApplyState(s, 0);
        }
        C3D_DrawArrays(c.prim, c.first, c.count);
    }
}

// ---- render thread --------------------------------------------------------------------------------------------
// A recorded frame, replayed into the screen(s) on the New 3DS's spare core (core 2) while the game runs its next
// frame; inline on an Old 3DS (no core for it).
struct Job { std::vector<Cmd> cmds, bottom; bool bottom_on = false; int arena = 0; float slider = 0, convergence = 600.0f; };
Job g_job;
Thread g_render = nullptr;
LightEvent g_job_ready, g_job_done;
bool g_pending = false;            // a job was handed over and isn't done yet (game thread)
volatile bool g_render_quit = false;

void RenderJob(const Job& job) {
    g_replay_convergence = job.convergence;
    const bool stereo = job.slider > 0.01f;
    const u64 t_wait = g_profile ? svcGetSystemTick() : 0;
    C3D_FrameBegin(0);   // waits for the GPU to finish the previous frame (not for a vblank: gles.cpp paces)
    if (g_profile) g_wait_ticks += svcGetSystemTick() - t_wait;
    auto clear_color = [](const std::vector<Cmd>& cmds) {
        return !cmds.empty() && cmds[0].kind == Kind::Clear ? ClearColorValue(cmds[0].clear_color) : 0x000000FFu;
    };
    // The top screen: a frame without draws for it (the game paused under the F1 overlay) leaves it as it is.
    g_replay_w = kScreenW;
    for (int eye = 0; !job.cmds.empty() && eye < (stereo ? 2 : 1); ++eye) {
        C3D_RenderTargetClear(g_target[eye], C3D_CLEAR_ALL, clear_color(job.cmds), 0xFFFFFF);
        C3D_FrameDrawOn(g_target[eye]);
        Replay(job.cmds, g_arena[job.arena], stereo ? (eye == 0 ? -job.slider : job.slider) : 0.0f);
    }
    if (job.bottom_on) {   // the F1 overlay (flat)
        g_replay_w = kBottomW;
        C3D_RenderTargetClear(g_target_bottom, C3D_CLEAR_ALL, clear_color(job.bottom), 0xFFFFFF);
        C3D_FrameDrawOn(g_target_bottom);
        Replay(job.bottom, g_arena[job.arena], 0.0f);
    }
    C3D_FrameEnd(0);
}

void RenderThread(void*) {
    for (;;) {
        LightEvent_Wait(&g_job_ready);
        if (g_render_quit) break;
        RenderJob(g_job);
        LightEvent_Signal(&g_job_done);
    }
}

void WaitRender() {   // game thread: the previous frame's replay is done (and the GPU has the frame before it)
    if (!g_pending) return;
    LightEvent_Wait(&g_job_done);
    g_pending = false;
}
}  // namespace

// ---- GL entry points --------------------------------------------------------------------------------------------
extern "C" {
GLenum glGetError(void) { GLenum e = S.error; S.error = GL_NO_ERROR; return e; }
const GLubyte* glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR: return reinterpret_cast<const GLubyte*>("MGS Mobile port");
        case GL_RENDERER: return reinterpret_cast<const GLubyte*>("citro3d (PICA200)");
        case GL_VERSION: return reinterpret_cast<const GLubyte*>("OpenGL ES-CM 1.1 (gl3ds)");
        case GL_EXTENSIONS: return reinterpret_cast<const GLubyte*>("GL_OES_framebuffer_object");
        default: return reinterpret_cast<const GLubyte*>("");
    }
}

void glEnable(GLenum cap);
void glDisable(GLenum cap);
static void SetCap(GLenum cap, bool on) {
    switch (cap) {
        case GL_TEXTURE_2D: S.unit[S.active].enabled = on; break;
        case GL_BLEND: S.blend_on = on; break;
        case GL_ALPHA_TEST: S.alpha_on = on; break;
        case GL_DEPTH_TEST: S.depth_on = on; break;
        case GL_CULL_FACE: S.cull_on = on; break;
        case GL_SCISSOR_TEST: S.scissor_on = on; break;
        case GL_FOG: S.fog_on = on; break;
        case GL_LIGHTING: S.lighting_on = on; break;
        case GL_COLOR_MATERIAL: S.color_material = on; break;
        case GL_DITHER: S.dither = on; break;
        default: break;   // lights, normalize, polygon offset, ...: no effect here
    }
}
void glEnable(GLenum cap) { SetCap(cap, true); }
void glDisable(GLenum cap) { SetCap(cap, false); }
GLboolean glIsEnabled(GLenum cap) {
    switch (cap) {
        case GL_TEXTURE_2D: return S.unit[S.active].enabled;
        case GL_BLEND: return S.blend_on;
        case GL_ALPHA_TEST: return S.alpha_on;
        case GL_DEPTH_TEST: return S.depth_on;
        case GL_CULL_FACE: return S.cull_on;
        case GL_SCISSOR_TEST: return S.scissor_on;
        case GL_FOG: return S.fog_on;
        case GL_LIGHTING: return S.lighting_on;
        case GL_COLOR_MATERIAL: return S.color_material;
        case GL_DITHER: return S.dither;
        default: return GL_FALSE;
    }
}

void glGetFloatv(GLenum p, GLfloat* v) {
    switch (p) {
        case GL_MODELVIEW_MATRIX: memcpy(v, S.mv.top().m, 64); break;
        case GL_PROJECTION_MATRIX: memcpy(v, S.proj.top().m, 64); break;
        case GL_TEXTURE_MATRIX: memcpy(v, S.unit[S.active].matrix.top().m, 64); break;
        case GL_CURRENT_COLOR: memcpy(v, S.color4, 16); break;
        case GL_COLOR_CLEAR_VALUE: memcpy(v, S.clear_color, 16); break;
        case GL_DEPTH_RANGE: v[0] = S.depth_near; v[1] = S.depth_far; break;
        case GL_ALPHA_TEST_REF: v[0] = S.alpha_ref; break;
        default: v[0] = 0; break;
    }
}
void glGetIntegerv(GLenum p, GLint* v) {
    switch (p) {
        case GL_VIEWPORT: memcpy(v, S.viewport, 16); break;
        case GL_SCISSOR_BOX: memcpy(v, S.scissor, 16); break;
        case GL_TEXTURE_BINDING_2D: v[0] = static_cast<GLint>(S.unit[S.active].bound); break;
        case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + S.active; break;
        case GL_CLIENT_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + S.client; break;
        case GL_MATRIX_MODE: v[0] = static_cast<GLint>(S.matrix_mode); break;
        case GL_BLEND_SRC: v[0] = static_cast<GLint>(S.blend_src); break;
        case GL_BLEND_DST: v[0] = static_cast<GLint>(S.blend_dst); break;
        case GL_SHADE_MODEL: v[0] = static_cast<GLint>(S.shade); break;
        case GL_UNPACK_ALIGNMENT: v[0] = S.unpack_align; break;
        case GL_PACK_ALIGNMENT: v[0] = S.pack_align; break;
        case GL_DEPTH_FUNC: v[0] = static_cast<GLint>(S.depth_func); break;
        case GL_MAX_TEXTURE_SIZE: v[0] = 1024; break;
        case GL_MAX_RENDERBUFFER_SIZE_OES: v[0] = 1024; break;
        case GL_MAX_TEXTURE_UNITS: v[0] = 2; break;
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES: v[0] = GL_RGBA; break;
        case GL_IMPLEMENTATION_COLOR_READ_TYPE_OES: v[0] = GL_UNSIGNED_BYTE; break;
        case GL_FRAMEBUFFER_BINDING_OES: v[0] = static_cast<GLint>(S.fbo_bound); break;
        default: v[0] = 0; break;
    }
}
void glGetBooleanv(GLenum p, GLboolean* v) {
    switch (p) {
        case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; ++i) v[i] = S.color_mask[i]; break;
        case GL_DEPTH_WRITEMASK: v[0] = S.depth_mask; break;
        default: v[0] = GL_FALSE; break;
    }
}
void glGetTexEnviv(GLenum, GLenum p, GLint* v) { v[0] = p == GL_TEXTURE_ENV_MODE ? S.unit[S.active].env_mode : 0; }

// matrices
void glMatrixMode(GLenum m) { S.matrix_mode = m; }
void glLoadIdentity(void) { CurrentStack().top() = Identity(); }
void glLoadMatrixf(const GLfloat* m) { memcpy(CurrentStack().top().m, m, 64); }
void glMultMatrixf(const GLfloat* m) { Mat b; memcpy(b.m, m, 64); CurrentStack().top() = Mul(CurrentStack().top(), b); }
void glPushMatrix(void) { Stack& s = CurrentStack(); if (s.s.size() < kStackDepth) s.s.push_back(s.top()); }
void glPopMatrix(void) { Stack& s = CurrentStack(); if (s.s.size() > 1) s.s.pop_back(); }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { Mat t = Identity(); t.m[12] = x; t.m[13] = y; t.m[14] = z; CurrentStack().top() = Mul(CurrentStack().top(), t); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { Mat t = Identity(); t.m[0] = x; t.m[5] = y; t.m[10] = z; CurrentStack().top() = Mul(CurrentStack().top(), t); }
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    const float a = angle * 3.14159265358979f / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
    Mat r = Identity();
    r.m[0] = t * x * x + c;     r.m[1] = t * x * y + s * z; r.m[2] = t * x * z - s * y;
    r.m[4] = t * x * y - s * z; r.m[5] = t * y * y + c;     r.m[6] = t * y * z + s * x;
    r.m[8] = t * x * z + s * y; r.m[9] = t * y * z - s * x; r.m[10] = t * z * z + c;
    CurrentStack().top() = Mul(CurrentStack().top(), r);
}
void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    Mat m{};
    m.m[0] = 2 * n / (r - l); m.m[5] = 2 * n / (t - b);
    m.m[8] = (r + l) / (r - l); m.m[9] = (t + b) / (t - b); m.m[10] = -(f + n) / (f - n); m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    CurrentStack().top() = Mul(CurrentStack().top(), m);
}
void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    Mat m = Identity();
    m.m[0] = 2 / (r - l); m.m[5] = 2 / (t - b); m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l); m.m[13] = -(t + b) / (t - b); m.m[14] = -(f + n) / (f - n);
    CurrentStack().top() = Mul(CurrentStack().top(), m);
}

// state
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.viewport[0] = x; S.viewport[1] = y; S.viewport[2] = w; S.viewport[3] = h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.scissor[0] = x; S.scissor[1] = y; S.scissor[2] = w; S.scissor[3] = h; }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.clear_color[0] = r; S.clear_color[1] = g; S.clear_color[2] = b; S.clear_color[3] = a; }
void glClearDepthf(GLclampf d) { S.clear_depth = d; }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.color4[0] = r; S.color4[1] = g; S.color4[2] = b; S.color4[3] = a; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.color_mask[0] = r; S.color_mask[1] = g; S.color_mask[2] = b; S.color_mask[3] = a; }
void glDepthMask(GLboolean f) { S.depth_mask = f; }
void glDepthFunc(GLenum f) { S.depth_func = f; }
void glDepthRangef(GLclampf n, GLclampf f) { S.depth_near = n; S.depth_far = f; }
void glAlphaFunc(GLenum f, GLclampf ref) { S.alpha_func = f; S.alpha_ref = ref; }
void glBlendFunc(GLenum s, GLenum d) { S.blend_src = s; S.blend_dst = d; }
void glCullFace(GLenum m) { S.cull_face = m; }
void glFrontFace(GLenum m) { S.front_face = m; }
void glShadeModel(GLenum m) { S.shade = m; }
void glHint(GLenum, GLenum) {}
void glPolygonOffset(GLfloat, GLfloat) {}   // (the asset viewer's outlines; no depth bias here)
void glLineWidth(GLfloat) {}
void glPointSize(GLfloat) {}
void glPixelStorei(GLenum p, GLint v) { if (p == GL_UNPACK_ALIGNMENT) S.unpack_align = std::max(1, v); else if (p == GL_PACK_ALIGNMENT) S.pack_align = std::max(1, v); }
void glFogf(GLenum, GLfloat) {}
void glFogfv(GLenum, const GLfloat*) {}
void glLightModelf(GLenum, GLfloat) {}
void glLightModelfv(GLenum, const GLfloat*) {}
void glLightfv(GLenum, GLenum, const GLfloat*) {}
void glMaterialfv(GLenum, GLenum, const GLfloat*) {}
void glFlush(void) {}
void glFinish(void) {}

void glClear(GLbitfield mask) {
    if (!g_ready) return;
    Vertex* v = Alloc(4);
    if (!v) return;
    const float quad[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        v[i] = Vertex{{quad[i][0], quad[i][1], 1, 1}, {S.clear_color[0], S.clear_color[1], S.clear_color[2], S.clear_color[3]}, {0, 0}, {0, 0}};
    }
    Cmd c;
    c.kind = Kind::Clear;
    c.st = TakeSnapshot();
    c.prim = GPU_TRIANGLE_STRIP;
    c.first = static_cast<uint32_t>(v - g_arena[g_cur].base);
    c.count = 4;
    c.clear_mask = mask;
    memcpy(c.clear_color, S.clear_color, sizeof(c.clear_color));
    Cmds().push_back(c);
}

// client arrays
void glEnableClientState(GLenum a);
static ClientArray* Array(GLenum a) {
    switch (a) {
        case GL_VERTEX_ARRAY: return &S.vertex;
        case GL_COLOR_ARRAY: return &S.color;
        case GL_NORMAL_ARRAY: return &S.normal;
        case GL_TEXTURE_COORD_ARRAY: return &S.texcoord[S.client];
        default: return nullptr;
    }
}
void glEnableClientState(GLenum a) { if (ClientArray* c = Array(a)) c->on = true; }
void glDisableClientState(GLenum a) { if (ClientArray* c = Array(a)) c->on = false; }
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* p) { S.vertex = {S.vertex.on, size, type, stride, p}; }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void* p) { S.color = {S.color.on, size, type, stride, p}; }
void glNormalPointer(GLenum type, GLsizei stride, const void* p) { S.normal = {S.normal.on, 3, type, stride, p}; }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ClientArray& t = S.texcoord[S.client]; t = {t.on, size, type, stride, p}; }
void glActiveTexture(GLenum t) { S.active = (t - GL_TEXTURE0) & 1; }
void glClientActiveTexture(GLenum t) { S.client = (t - GL_TEXTURE0) & 1; }

void glDrawArrays(GLenum mode, GLint first, GLsizei count) { Record(mode, static_cast<uint32_t>(count), nullptr, static_cast<uint32_t>(first)); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    std::vector<uint32_t> idx(static_cast<size_t>(count));
    for (GLsizei i = 0; i < count; ++i)
        idx[i] = type == GL_UNSIGNED_BYTE ? static_cast<const uint8_t*>(indices)[i] : static_cast<const uint16_t*>(indices)[i];
    Record(mode, static_cast<uint32_t>(count), &idx, 0);
}

// textures
void glGenTextures(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) { ids[i] = S.next_name++; g_textures[ids[i]]; } }
void glDeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) {
        auto it = g_textures.find(ids[i]);
        if (it == g_textures.end()) continue;
        FreeTex(it->second);
        g_textures.erase(it);
        for (TexUnit& u : S.unit) if (u.bound == ids[i]) u.bound = 0;
    }
}
GLboolean glIsTexture(GLuint id) { return g_textures.count(id) ? GL_TRUE : GL_FALSE; }
void glBindTexture(GLenum, GLuint id) { S.unit[S.active].bound = id; if (id) g_textures[id]; }
void glTexParameteri(GLenum, GLenum p, GLint v) {
    Texture* t = Bound();
    if (!t) return;
    switch (p) {
        case GL_TEXTURE_MIN_FILTER: t->min = v; break;
        case GL_TEXTURE_MAG_FILTER: t->mag = v; break;
        case GL_TEXTURE_WRAP_S: t->wrap_s = v; break;
        case GL_TEXTURE_WRAP_T: t->wrap_t = v; break;
        default: return;
    }
    ApplyTexParams(*t);
}
void glTexParameterf(GLenum target, GLenum p, GLfloat v) { glTexParameteri(target, p, static_cast<GLint>(v)); }
void glTexEnvi(GLenum, GLenum p, GLint v) { if (p == GL_TEXTURE_ENV_MODE) S.unit[S.active].env_mode = v; }
void glTexEnvf(GLenum target, GLenum p, GLfloat v) { glTexEnvi(target, p, static_cast<GLint>(v)); }

void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void* pixels) {
    Texture* t = Bound();
    if (!t || w <= 0 || h <= 0) return;
    if (level > 0) {   // a mip level: into the chain allocated with level 0, if it fits there
        if (!t->ready || level > t->mip_alloc || !pixels || w != std::max(1, t->tex.width >> level) || h != std::max(1, t->tex.height >> level)) return;
        Store(*t, 0, 0, w, h, static_cast<const uint8_t*>(pixels), format, type, level);
        if (level == t->mip_done + 1) { t->mip_done = level; ApplyTexParams(*t); }
        return;
    }
    FreeTex(*t);
    t->shadow.clear();
    t->w = w; t->h = h;
    t->mip_done = 0;
    const int tw = Pot(w), th = Pot(h);
    const bool pot = tw == w && th == h;
    const GPU_TEXCOLOR fmt = pot ? GpuFormat(type) : GPU_RGBA8;
    // Power of two: room for the game's mip levels (down to 8x8, the PICA's smallest).
    t->mip_alloc = pot ? C3D_TexCalcMaxLevel(tw, th) : 0;
    const bool ok = pot ? C3D_TexInitMipmap(&t->tex, static_cast<u16>(tw), static_cast<u16>(th), fmt)
                        : C3D_TexInit(&t->tex, static_cast<u16>(tw), static_cast<u16>(th), fmt);
    if (!ok) {
        std::cout << "[GL3DS] Out of linear memory for a " << tw << "x" << th << " texture" << std::endl;
        return;
    }
    t->ready = true;
    memset(t->tex.data, 0, C3D_TexCalcTotalSize(t->tex.size, t->mip_alloc));
    if (pixels) Store(*t, 0, 0, w, h, static_cast<const uint8_t*>(pixels), format, type);
    else GSPGPU_FlushDataCache(t->tex.data, C3D_TexCalcTotalSize(t->tex.size, t->mip_alloc));
    ApplyTexParams(*t);
}
void glTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels) {
    Texture* t = Bound();
    if (!t || !t->ready || level != 0 || !pixels) return;
    if (t->tex.fmt != GpuFormat(type) && t->tex.width == t->w && t->tex.height == t->h) return;   // (format changes: not supported)
    Store(*t, x, y, w, h, static_cast<const uint8_t*>(pixels), format, type);
}
void glCopyTexImage2D(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) {}
void glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*) {}
void glReadPixels(GLint, GLint, GLsizei w, GLsizei h, GLenum, GLenum type, void* out) {
    memset(out, 0, static_cast<size_t>(w) * h * (type == GL_UNSIGNED_BYTE ? 4 : 2));
}

// Framebuffer objects: gles.cpp's render target is the top screen itself.
void glGenFramebuffersOES(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = S.next_name++; }
void glBindFramebufferOES(GLenum, GLuint id) { S.fbo_bound = id; }
void glGenRenderbuffersOES(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = S.next_name++; }
void glBindRenderbufferOES(GLenum, GLuint id) { S.rb_bound = id; }
void glRenderbufferStorageOES(GLenum, GLenum, GLsizei, GLsizei) {}
void glFramebufferRenderbufferOES(GLenum, GLenum, GLenum, GLuint) {}
GLenum glCheckFramebufferStatusOES(GLenum) { return GL_FRAMEBUFFER_COMPLETE_OES; }
}  // extern "C"

// ---- host side ----------------------------------------------------------------------------------------------------
bool GL3DS::Init() {
    if (g_ready) return true;
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) { std::cout << "[GL3DS] C3D_Init failed" << std::endl; return false; }
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    for (int eye = 0; eye < (g_stereo ? 2 : 1); ++eye) {
        g_target[eye] = C3D_RenderTargetCreate(kScreenH, kScreenW, GPU_RB_RGBA8, C3D_DEPTHTYPE(GPU_RB_DEPTH24_STENCIL8));
        if (!g_target[eye]) { std::cout << "[GL3DS] No render target" << std::endl; return false; }
        C3D_RenderTargetSetOutput(g_target[eye], GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, flags);
    }
    // The bottom screen (the F1 overlay): RGB565 like the console that shares it.
    g_target_bottom = C3D_RenderTargetCreate(kScreenH, kBottomW, GPU_RB_RGBA8, C3D_DEPTHTYPE(GPU_RB_DEPTH24_STENCIL8));
    if (!g_target_bottom) { std::cout << "[GL3DS] No bottom render target" << std::endl; return false; }
    C3D_RenderTargetSetOutput(g_target_bottom, GFX_BOTTOM, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    g_dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<u8*>(gl3ds_shbin)), gl3ds_shbin_size);
    shaderProgramInit(&g_program);
    shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_program);
    g_loc_proj = shaderInstanceGetUniformLocation(g_program.vertexShader, "projection");
    g_loc_mv = shaderInstanceGetUniformLocation(g_program.vertexShader, "modelView");
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);   // position
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4);   // colour
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);   // texcoord 0
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);   // texcoord 1
    for (Arena& a : g_arena) {
        a.base = static_cast<Vertex*>(linearAlloc(kArenaBytes));
        if (!a.base) { std::cout << "[GL3DS] Out of linear memory for the vertex arena" << std::endl; return false; }
    }
    g_ready = true;
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    // The replay on its own core, a little above the game's priority: core 2 on a New 3DS (its extra application
    // core). On an Old 3DS / 2DS inline: core 1 (the system core, APT_SetAppCpuTimeLimit) is where the SD card is
    // served, and a replay there made each file open 70-150 ms instead of 15-40 with no faster play (2DS,
    // 2026-10-17). MGS_CORE1_LIMIT=<percent> (5-80) still tries it there.
    int core = is_new ? 2 : -1;
    const int limit = getenv("MGS_CORE1_LIMIT") ? std::clamp(atoi(getenv("MGS_CORE1_LIMIT")), 5, 80) : 0;
    if (!is_new && limit && R_SUCCEEDED(APT_SetAppCpuTimeLimit(limit))) core = 1;
    if (core > 0) {
        LightEvent_Init(&g_job_ready, RESET_ONESHOT);
        LightEvent_Init(&g_job_done, RESET_ONESHOT);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_render = threadCreate(RenderThread, nullptr, 64 * 1024, prio > 0x18 ? prio - 1 : prio, core, false);
    }
    std::cout << "[GL3DS] Render thread: " << (g_render ? (core == 2 ? "core 2" : "core 1, " + std::to_string(limit) + "%") : "none (replay inline)") << std::endl;
    std::cout << "[GL3DS] citro3d ready: top screen 400x240, " << (g_stereo ? "stereo up to the 3D slider" : "no stereo") << std::endl;
    return true;
}

void* GL3DS::Proc(const char* name) {
    struct Entry { const char* name; void* fn; };
    static const Entry kProcs[] = {
        {"glActiveTexture", reinterpret_cast<void*>(&glActiveTexture)},
        {"glClientActiveTexture", reinterpret_cast<void*>(&glClientActiveTexture)},
        {"glGenFramebuffers", reinterpret_cast<void*>(&glGenFramebuffersOES)},
        {"glBindFramebuffer", reinterpret_cast<void*>(&glBindFramebufferOES)},
        {"glGenRenderbuffers", reinterpret_cast<void*>(&glGenRenderbuffersOES)},
        {"glBindRenderbuffer", reinterpret_cast<void*>(&glBindRenderbufferOES)},
        {"glRenderbufferStorage", reinterpret_cast<void*>(&glRenderbufferStorageOES)},
        {"glFramebufferRenderbuffer", reinterpret_cast<void*>(&glFramebufferRenderbufferOES)},
        {"glCheckFramebufferStatus", reinterpret_cast<void*>(&glCheckFramebufferStatusOES)},
    };
    std::string n = name;
    for (const char* suffix : {"OES", "EXT", "ARB"})
        if (n.size() > 3 && n.compare(n.size() - 3, 3, suffix) == 0) { n.resize(n.size() - 3); break; }
    for (const Entry& e : kProcs) if (n == e.name) return e.fn;
    return nullptr;   // (blits, multisampling: not available, so gles.cpp renders straight to the screen)
}

void GL3DS::SetStereo(bool on) { if (!g_ready) g_stereo = on; }

void GL3DS::SetStereoStrength(int percent) { g_stereo_percent = std::clamp(percent, 0, 200); }

void GL3DS::TakeStats(double& record_ms, double& wait_ms, double& end_ms) {
    record_ms = g_record_ticks / CPU_TICKS_PER_MSEC; wait_ms = g_wait_ticks / CPU_TICKS_PER_MSEC; end_ms = g_end_ticks / CPU_TICKS_PER_MSEC;
    g_record_ticks = g_wait_ticks = g_end_ticks = 0;
}

void GL3DS::EndFrame() {
    if (!g_ready) return;
    const u64 t_end = g_profile ? svcGetSystemTick() : 0;
    // The vertices were written through the CPU cache: write them back before the GPU reads them.
    if (g_arena[g_cur].used) GSPGPU_FlushDataCache(g_arena[g_cur].base, g_arena[g_cur].used * sizeof(Vertex));
    WaitRender();
    // The GPU is done with frame k - 2 (the replay of k - 1 began by waiting for it): free what was deleted then.
    std::vector<C3D_Tex>& done = g_deferred_free[(g_cur + 1) % kArenas];
    for (C3D_Tex& t : done) C3D_TexDelete(&t);
    done.clear();
    if (g_frame_near < 1e29f) g_convergence += (std::clamp(g_frame_near, 40.0f, 4000.0f) - g_convergence) * 0.15f;
    g_frame_near = 1e30f;
    g_job.cmds.swap(g_cmds);
    g_job.bottom.swap(g_cmds_bottom);
    g_job.bottom_on = g_bottom_on;
    g_job.arena = g_cur;
    g_job.slider = g_stereo ? osGet3DSliderState() * g_stereo_percent / 100.0f : 0.0f;
    g_job.convergence = g_convergence;
    if (g_render) { g_pending = true; LightEvent_Signal(&g_job_ready); }
    else RenderJob(g_job);
    // Record the next frame into the next arena.
    g_cmds.clear();
    g_cmds_bottom.clear();
    g_cur = (g_cur + 1) % kArenas;
    g_arena[g_cur].used = 0;
    if (g_profile) g_end_ticks += svcGetSystemTick() - t_end;
}

void GL3DS::BeginBottom() { g_rec_bottom = true; }
void GL3DS::EndBottom() { g_rec_bottom = false; }

void GL3DS::SetBottomScreen(bool on) {
    if (!g_ready || on == g_bottom_on) return;
    WaitRender();   // (no frame in flight while the bottom screen changes hands)
    g_bottom_on = on;
    // Ours: double-buffered like the top. The console (main_3ds.cpp) wants it single-buffered (consoleInit).
    gfxSetDoubleBuffering(GFX_BOTTOM, on);
}

void GL3DS::Shutdown() {
    if (!g_render) return;
    WaitRender();
    g_render_quit = true;
    LightEvent_Signal(&g_job_ready);
    threadJoin(g_render, U64_MAX);
    threadFree(g_render);
    g_render = nullptr;
}
