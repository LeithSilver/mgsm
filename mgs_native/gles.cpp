#include "glheaders.h"
#include "gles.h"
#include "loader.h"
#include "assetviewer.h"
#include "custommodel.h"
#include "settings.h"
#include "textures.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>

// ---- GL constants not in the Windows SDK's GL 1.1 header -------------------------------
#define GL_FIXED                    0x140C
#define GL_BGRA_EXT                 0x80E1
#define GL_TEXTURE0                 0x84C0
#define GL_MAX_TEXTURE_UNITS        0x84E2
#define GL_UNSIGNED_SHORT_4_4_4_4   0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1   0x8034
#define GL_UNSIGNED_SHORT_5_6_5     0x8363
#define GL_RGB_SCALE                0x8573
#define GL_GENERATE_MIPMAP          0x8191
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS     0x86A3
#define GL_MATRIX_PALETTE_OES       0x8840
#define GL_MATRIX_INDEX_ARRAY_OES   0x8844
#define GL_WEIGHT_ARRAY_OES         0x86AD
#define GL_POINT_SIZE_ARRAY_OES     0x8B9C
#define GL_MAX_PALETTE_MATRICES_OES 0x8842
#define GL_MAX_VERTEX_UNITS_OES     0x86A4
#define GL_PALETTE4_RGB8_OES        0x8B90
#define GL_PALETTE8_RGB5_A1_OES     0x8B99
#define GL_IMPLEMENTATION_COLOR_READ_TYPE_OES   0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES 0x8B9B
#define GL_FRAMEBUFFER              0x8D40
#define GL_RENDERBUFFER             0x8D41
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_DEPTH_ATTACHMENT         0x8D00
#define GL_STENCIL_ATTACHMENT       0x8D20
#define GL_DEPTH24_STENCIL8         0x88F0
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5

typedef void   (APIENTRY* PFN_ActiveTexture)(GLenum);
typedef void   (APIENTRY* PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY* PFN_BindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY* PFN_GenRenderbuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY* PFN_BindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY* PFN_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY* PFN_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY* PFN_CheckFramebufferStatus)(GLenum);
typedef void   (APIENTRY* PFN_RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY* PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void   (APIENTRY* PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_MAX_SAMPLES              0x8D57
#define GL_MAX_RENDERBUFFER_SIZE    0x84E8

static PFN_ActiveTexture            p_glActiveTexture;
static PFN_ActiveTexture            p_glClientActiveTexture;
static PFN_GenFramebuffers          p_glGenFramebuffers;
static PFN_BindFramebuffer          p_glBindFramebuffer;
static PFN_GenRenderbuffers         p_glGenRenderbuffers;
static PFN_BindRenderbuffer         p_glBindRenderbuffer;
static PFN_RenderbufferStorage      p_glRenderbufferStorage;
static PFN_FramebufferRenderbuffer  p_glFramebufferRenderbuffer;
static PFN_CheckFramebufferStatus   p_glCheckFramebufferStatus;
static PFN_RenderbufferStorageMultisample p_glRenderbufferStorageMultisample;
static PFN_BlitFramebuffer          p_glBlitFramebuffer;
[[maybe_unused]] static PFN_FramebufferTexture2D p_glFramebufferTexture2D;   // (Vita: the colour attachment is a texture)

static GLES::Host g_host;   // context + function loader from the platform layer (main.cpp: SDL)

// Core name, then EXT, then OES. ES 1.1 only has the OES ones (Android can hand out ES 2 core entry points that
// don't work in an ES 1 context), so it uses the OES name alone when there is one.
template <class T> static T LoadGL(const char* a, const char* b = nullptr, const char* c = nullptr) {
#if defined(MGS_GLES) && !defined(MGS_VITA)   // (vitaGL has the core names)
    if (c) return reinterpret_cast<T>(GLES::Proc(c));
#endif
    void* p = GLES::Proc(a);
    if (!p && b) p = GLES::Proc(b);
    if (!p && c) p = GLES::Proc(c);
    return reinterpret_cast<T>(p);
}

// glReadPixels into 0xAARRGGBB words. ES only guarantees RGBA bytes, so it swaps red and blue itself.
static void ReadPixelsBGRA(int w, int h, uint32_t* out) {
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
#ifdef MGS_GLES
    // The driver's own read format first (GL_IMPLEMENTATION_COLOR_READ_FORMAT / _TYPE): for the RGB565 fallback
    // render target that is RGB565, while RGBA bytes go through a slow conversion (PowerVR: 122 ms a frame at
    // 2856x1279 against a few ms).
    GLint fmt = 0, type = 0;
    glGetIntegerv(0x8B9B /*GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES*/, &fmt);
    glGetIntegerv(0x8B9A /*GL_IMPLEMENTATION_COLOR_READ_TYPE_OES*/, &type);
    if (fmt == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        static std::vector<uint16_t> px16;
        px16.resize(static_cast<size_t>(w) * h + 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 2);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px16.data());
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
            const uint32_t s = px16[i], r = (s >> 11) & 31, g = (s >> 5) & 63, b = s & 31;
            out[i] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        return;
    }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        const uint32_t p = out[i];
        out[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
    }
#else
    glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, out);
#endif
}

// ---- GLStateSave / GLDrawFullscreenTexture (glheaders.h) ---------------------------------------------------
#ifdef MGS_GLES
static const GLenum kSavedCaps[] = { GL_DEPTH_TEST, GL_BLEND, GL_ALPHA_TEST, GL_FOG, GL_LIGHTING, GL_SCISSOR_TEST,
    GL_STENCIL_TEST, GL_CULL_FACE, GL_COLOR_MATERIAL, GL_NORMALIZE, GL_RESCALE_NORMAL, GL_POLYGON_OFFSET_FILL, GL_DITHER };
static_assert(sizeof(kSavedCaps) / sizeof(kSavedCaps[0]) == 13, "GLStateSave::kCaps");

GLStateSave::GLStateSave() {
    for (int i = 0; i < kCaps; ++i) caps_[i] = glIsEnabled(kSavedCaps[i]);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client_);
    for (int u = 0; u < 2; ++u) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + u);
        else if (u) break;
        tex2d_[u] = glIsEnabled(GL_TEXTURE_2D);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_[u]);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env_mode_[u]);
    }
    if (p_glActiveTexture) p_glActiveTexture(static_cast<GLenum>(active_));
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask_);
    glGetIntegerv(GL_VIEWPORT, viewport_);
    glGetIntegerv(GL_SCISSOR_BOX, scissor_);
    glGetIntegerv(GL_BLEND_SRC, &blend_src_);
    glGetIntegerv(GL_BLEND_DST, &blend_dst_);
    glGetIntegerv(GL_SHADE_MODEL, &shade_);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_);
    glGetIntegerv(GL_MATRIX_MODE, &matrix_mode_);
    glGetFloatv(GL_CURRENT_COLOR, color_);
}

GLStateSave::~GLStateSave() {
    for (int i = 0; i < kCaps; ++i) { if (caps_[i]) glEnable(kSavedCaps[i]); else glDisable(kSavedCaps[i]); }
    for (int u = 1; u >= 0; --u) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + u);
        else if (u) continue;
        if (tex2d_[u]) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound_[u]));
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode_[u]);
    }
    if (p_glActiveTexture) p_glActiveTexture(static_cast<GLenum>(active_));
    if (p_glClientActiveTexture) p_glClientActiveTexture(static_cast<GLenum>(client_));
    glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
    glDepthMask(depth_mask_);
    glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
    glBlendFunc(static_cast<GLenum>(blend_src_), static_cast<GLenum>(blend_dst_));
    glShadeModel(static_cast<GLenum>(shade_));
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_);
    glMatrixMode(static_cast<GLenum>(matrix_mode_));
    glColor4f(color_[0], color_[1], color_[2], color_[3]);
}
#else
GLStateSave::GLStateSave() { glPushAttrib(GL_ALL_ATTRIB_BITS); glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS); }
GLStateSave::~GLStateSave() { glPopClientAttrib(); glPopAttrib(); }
#endif

void GLDrawFullscreenTexture(GLuint tex, bool flip_v) {
    if (p_glActiveTexture) { p_glActiveTexture(GL_TEXTURE0 + 1); glDisable(GL_TEXTURE_2D); p_glActiveTexture(GL_TEXTURE0); }
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_FOG);
    glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    static const GLfloat pos[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    static const GLfloat uv_down[] = { 0, 1, 1, 1, 0, 0, 1, 0 }, uv_up[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    if (p_glClientActiveTexture) { p_glClientActiveTexture(GL_TEXTURE0 + 1); glDisableClientState(GL_TEXTURE_COORD_ARRAY); p_glClientActiveTexture(GL_TEXTURE0); }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, flip_v ? uv_down : uv_up);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

// ---- helpers -----------------------------------------------------------------------------
static inline float Fx(uint32_t v) { return static_cast<int32_t>(v) / 65536.0f; }
static inline float AsF(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

namespace {

struct ClientArray {
    bool enabled = false;
    int size = 4;
    uint32_t type = GL_FLOAT;
    uint32_t stride = 0;
    uint32_t ptr = 0;
};

struct State {
    bool tried = false, ok = false;   // context made current (EnsureContext)
    GLuint fbo = 0, rb_color = 0, rb_depth = 0;   // the game's render target (multisampled with MSAA)
    GLuint tex_color = 0;                            // Vita: its colour as a texture instead of rb_color
    GLuint resolve_fbo = 0, rb_resolve = 0;          // MSAA + supersampling: same-size single-sample copy
    GLuint out_fbo = 0, rb_out = 0;                  // window-size output (overlay, readback) when AA is on
    int msaa = 0, ss = 1;                            // effective sample count / supersampling factor
    bool native = false;                             // render at the guest screen size, scaled up (nearest)
    bool Scaled() const { return ss > 1 || native; } // render target size != output size

    ClientArray vertex, normal, color, texcoord[2];
    int client_unit = 0;
    uint32_t matrix_mode = GL_MODELVIEW;       // as the guest sees it (may be GL_MATRIX_PALETTE_OES)
    uint32_t host_matrix_mode = GL_MODELVIEW;  // last mode actually set on the host

    // GL_OES_matrix_palette, emulated: palette matrices replace the modelview for skinned draws.
    // Vertices are blended on the host (eye space) and drawn with an identity modelview.
    static constexpr int kPaletteSize = 32;
    static constexpr int kVertexUnits = 4;
    float palette[kPaletteSize][16];
    int cur_palette = 0;
    bool palette_enabled = false;
    ClientArray mindex, weight;

    State() { for (auto& m : palette) for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
    int surf_w = 240, surf_h = 320;   // guest screen
    int fbo_w = 480, fbo_h = 640;     // host render target (output size x supersampling factor)
    int out_w = 480, out_h = 640;     // output frame size: the window's (so the overlay and touch controls reach its edges)
    int game_x = 0, game_y = 0, game_w = 480, game_h = 640;   // the game's box in the output (GL: from the bottom left)
    // Render-target pixels per guest pixel, and the box's offset in the render target. Native resolution renders the
    // game alone at the guest size; otherwise the target is the output (x supersampling) with the game in its box.
    float GameScaleX() const { return native ? 1.0f : float(game_w * ss) / surf_w; }
    float GameScaleY() const { return native ? 1.0f : float(game_h * ss) / surf_h; }
    int GameX() const { return native ? 0 : game_x * ss; }
    int GameY() const { return native ? 0 : game_y * ss; }
    int viewport[4] = {0, 0, 240, 320};
    int scissor[4] = {0, 0, 240, 320};
    int unpack_align = 4;
    std::unordered_map<std::string, uint32_t> strings;
    std::unordered_set<std::string> warned;
    uint32_t frames = 0;
};
State S;

std::function<void(const std::string&)> g_warning_cb;

// Something the GL layer can't do (logged once; also shown on screen in debug mode, see GLES::SetWarningCallback).
void WarnOnce(const std::string& what) {
    if (!S.warned.insert(what).second) return;
    std::cout << "[GLES] " << what << std::endl;
    if (g_warning_cb) g_warning_cb(what);
}
// Informational, once per message: debug-mode console only.
void InfoOnce(const std::string& what) {
    if (S.warned.insert(what).second && VerboseLog()) std::cout << "[GLES] " << what << std::endl;
}

uint32_t Arg(Cpu* uc, const uint32_t r[4], int i) {
    if (i < 4) return r[i];
    uint32_t sp = 0, v = 0;
    uc->RegRead(Cpu::REG_SP, &sp);
    uc->MemRead(sp + 4 * (i - 4), &v, 4);
    return v;
}

std::vector<uint8_t> ReadMem(Cpu* uc, uint32_t addr, size_t len) {
    std::vector<uint8_t> v(len);
    if (len && addr && uc->MemRead(addr, v.data(), len) != Cpu::OK) {
        WarnOnce("bad guest read");
        std::fill(v.begin(), v.end(), 0);
    }
    return v;
}

void ReadFixedVec(Cpu* uc, uint32_t addr, int n, float* out) {
    int32_t tmp[16] = {};
    uc->MemRead(addr, tmp, 4 * n);
    for (int i = 0; i < n; ++i) out[i] = tmp[i] / 65536.0f;
}

int TypeSize(uint32_t type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        default: return 4; // GL_FIXED, GL_FLOAT
    }
}

// Convert one guest array element to floats.
inline void Fetch(const uint8_t* p, uint32_t type, int n, bool normalized, float* out) {
    for (int i = 0; i < n; ++i) {
        switch (type) {
            case GL_BYTE:           out[i] = normalized ? reinterpret_cast<const int8_t*>(p)[i] / 127.0f : reinterpret_cast<const int8_t*>(p)[i]; break;
            case GL_UNSIGNED_BYTE:  out[i] = normalized ? p[i] / 255.0f : p[i]; break;
            case GL_SHORT:          { int16_t s; memcpy(&s, p + 2 * i, 2); out[i] = normalized ? s / 32767.0f : s; break; }
            case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p + 2 * i, 2); out[i] = normalized ? s / 65535.0f : s; break; }
            case GL_FIXED:          { int32_t x; memcpy(&x, p + 4 * i, 4); out[i] = x / 65536.0f; break; }
            default:                { memcpy(&out[i], p + 4 * i, 4); break; }
        }
    }
}

// Copy vertices [lo, hi] of one client array out of guest memory as floats (tightly packed).
std::vector<float> GatherArray(Cpu* uc, const ClientArray& a, int comps, uint32_t lo, uint32_t hi, bool normalized) {
    const uint32_t elem = TypeSize(a.type) * a.size;
    const uint32_t stride = a.stride ? a.stride : elem;
    const uint32_t count = hi - lo + 1;
    std::vector<uint8_t> raw = ReadMem(uc, a.ptr + lo * stride, static_cast<size_t>(count - 1) * stride + elem);
    std::vector<float> out(static_cast<size_t>(count) * comps, 0.0f);
    for (uint32_t i = 0; i < count; ++i) {
        float* o = &out[static_cast<size_t>(i) * comps];
        Fetch(raw.data() + static_cast<size_t>(i) * stride, a.type, std::min(a.size, comps), normalized, o);
        if (comps == 4 && a.size == 3) o[3] = 1.0f; // (colours are always size 4 in ES)
    }
    return out;
}

// Column-major 4x4: m = m * b (as glMultMatrix does)
void MulMatrix(float* m, const float* b) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = m[row] * b[c * 4] + m[4 + row] * b[c * 4 + 1] + m[8 + row] * b[c * 4 + 2] + m[12 + row] * b[c * 4 + 3];
    memcpy(m, r, sizeof(r));
}

void RotationMatrix(float angle_deg, float x, float y, float z, float* m) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    float a = angle_deg * 3.14159265358979f / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
    float r[16] = { t * x * x + c,     t * x * y + s * z, t * x * z - s * y, 0,
                    t * x * y - s * z, t * y * y + c,     t * y * z + s * x, 0,
                    t * x * z + s * y, t * y * z - s * x, t * z * z + c,     0,
                    0, 0, 0, 1 };
    memcpy(m, r, sizeof(r));
}

// Blend positions/normals of vertices [lo, hi] with the palette (OES_matrix_palette semantics):
// v' = sum_k w_k * P[idx_k] * v. Result positions are 3-component eye-space coordinates.
void SkinVertices(Cpu* uc, uint32_t lo, uint32_t hi, std::vector<float>& pos, int& vsize, std::vector<float>* nrm) {
    const int units = std::min(S.mindex.size, S.weight.size);
    std::vector<float> idx = GatherArray(uc, S.mindex, units, lo, hi, false);
    std::vector<float> wgt = GatherArray(uc, S.weight, units, lo, hi, false);
    const uint32_t n = hi - lo + 1;
    std::vector<float> out(static_cast<size_t>(n) * 3);
    for (uint32_t i = 0; i < n; ++i) {
        const float* p = &pos[static_cast<size_t>(i) * vsize];
        float v[4] = {p[0], p[1], vsize > 2 ? p[2] : 0.0f, vsize > 3 ? p[3] : 1.0f};
        float o[3] = {0, 0, 0}, on[3] = {0, 0, 0};
        float* nv = nrm ? &(*nrm)[static_cast<size_t>(i) * 3] : nullptr;
        for (int k = 0; k < units; ++k) {
            float w = wgt[static_cast<size_t>(i) * units + k];
            if (w == 0.0f) continue;
            int m = static_cast<int>(idx[static_cast<size_t>(i) * units + k]);
            if (m < 0 || m >= State::kPaletteSize) continue;
            const float* M = S.palette[m];
            for (int r = 0; r < 3; ++r) o[r] += w * (M[r] * v[0] + M[4 + r] * v[1] + M[8 + r] * v[2] + M[12 + r] * v[3]);
            if (nv) for (int r = 0; r < 3; ++r) on[r] += w * (M[r] * nv[0] + M[4 + r] * nv[1] + M[8 + r] * nv[2]);
        }
        memcpy(&out[static_cast<size_t>(i) * 3], o, sizeof(o));
        if (nv) memcpy(nv, on, sizeof(on));
    }
    pos.swap(out);
    vsize = 3;
}

bool g_dump_draw = false;
const char* TypeName(uint32_t t) {
    switch (t) { case GL_BYTE: return "B"; case GL_UNSIGNED_BYTE: return "UB"; case GL_SHORT: return "S";
                 case GL_UNSIGNED_SHORT: return "US"; case GL_FIXED: return "X"; case GL_FLOAT: return "F"; default: return "?"; }
}

std::atomic<bool> g_hide_2d{false};

// True when the host projection has no perspective (w row 0,0,0,1): IwGx's 2D / screen-space draws.
bool Is2DProjection() {
    float pr[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pr);
    return pr[3] == 0.0f && pr[7] == 0.0f && pr[11] == 0.0f && pr[15] != 0.0f;
}

void Draw(Cpu* uc, uint32_t mode, uint32_t first, uint32_t count, uint32_t idx_type, uint32_t idx_ptr) {
    if (g_hide_2d && Is2DProjection()) return;
    if (!S.ok || count == 0 || !S.vertex.enabled) return;

#ifdef MGS_3DS
    // Native backend (README.md section 6.28): guest addresses are host addresses, so the game's own arrays and
    // indices go to the GL layer as they are (it converts each vertex once, any type). The float copies below
    // are only needed for skinning, the custom player model, frame captures and GL dumps.
    const bool skinned_draw = S.palette_enabled && S.mindex.enabled && S.weight.enabled;
    if (uc->IsNative() && !g_dump_draw && !skinned_draw && !CustomModel::Covers() && !AssetViewer::Recording()) {
        auto host = [](uint32_t a) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(a)); };
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(S.vertex.size, S.vertex.type, S.vertex.stride, host(S.vertex.ptr));
        if (S.color.enabled) { glEnableClientState(GL_COLOR_ARRAY); glColorPointer(S.color.size, S.color.type, S.color.stride, host(S.color.ptr)); }
        else glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);   // (no lighting in the GL layer)
        for (int u = 0; u < 2; ++u) {
            p_glClientActiveTexture(GL_TEXTURE0 + u);
            if (S.texcoord[u].enabled) {
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(S.texcoord[u].size, S.texcoord[u].type, S.texcoord[u].stride, host(S.texcoord[u].ptr));
            } else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        p_glClientActiveTexture(GL_TEXTURE0 + S.client_unit);
        if (idx_ptr || idx_type) glDrawElements(mode, count, idx_type == GL_UNSIGNED_BYTE ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT, host(idx_ptr));
        else glDrawArrays(mode, first, count);
        return;
    }
#endif

    std::vector<uint16_t> indices;   // rebased to 0; 16-bit like the guest's (ES has no 32-bit indices)
    uint32_t lo = first, hi = first + count - 1;
    if (idx_ptr || idx_type) {
        const int isz = (idx_type == GL_UNSIGNED_BYTE) ? 1 : 2;
        std::vector<uint8_t> raw = ReadMem(uc, idx_ptr, static_cast<size_t>(count) * isz);
        indices.resize(count);
        lo = 0xFFFFFFFF; hi = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t v = (isz == 1) ? raw[i] : (raw[2 * i] | (raw[2 * i + 1] << 8));
            indices[i] = static_cast<uint16_t>(v);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        for (auto& v : indices) v = static_cast<uint16_t>(v - lo);
    }

    std::vector<float> pos = GatherArray(uc, S.vertex, S.vertex.size, lo, hi, false);
    if (g_dump_draw) {
        auto arr = [](const char* n, const ClientArray& a) {
            if (!a.enabled) return;
            std::cout << " " << n << "=" << a.size << TypeName(a.type) << "/s" << a.stride << "@0x" << std::hex << a.ptr << std::dec;
        };
        std::cout << "[GLDump]   draw mode=" << mode << " n=" << count << " range=" << lo << ".." << hi;
        arr("V", S.vertex); arr("N", S.normal); arr("C", S.color); arr("T0", S.texcoord[0]); arr("T1", S.texcoord[1]);
        std::cout << std::endl << "[GLDump]     v:";
        for (uint32_t i = 0; i < std::min<uint32_t>(3, hi - lo + 1); ++i) {
            std::cout << " (";
            for (int c = 0; c < S.vertex.size; ++c) std::cout << (c ? "," : "") << pos[i * S.vertex.size + c];
            std::cout << ")";
        }
        float mv[16], pr[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv); glGetFloatv(GL_PROJECTION_MATRIX, pr);
        if (S.color.enabled) {   // first vertices' colours
            const std::vector<float> c4 = GatherArray(uc, S.color, 4, lo, hi, true);
            std::cout << std::endl << "[GLDump]     c:";
            for (size_t k = 0; k < std::min<size_t>(c4.size(), 16); ++k) std::cout << (k % 4 ? "," : " ") << c4[k];
        }
        std::cout << std::endl << "[GLDump]     MV:"; for (float f : mv) std::cout << " " << f;
        std::cout << std::endl << "[GLDump]     P:"; for (float f : pr) std::cout << " " << f;
        // Screen-space bounding box (surface pixels, y down) of all vertices in the range
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        const int vs = S.vertex.size;
        for (uint32_t i = 0; i <= hi - lo; ++i) {
            float v[4] = {pos[i * vs], pos[i * vs + 1], vs > 2 ? pos[i * vs + 2] : 0.0f, 1.0f}, e[4], c[4];
            for (int r = 0; r < 4; ++r) e[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2] + mv[12 + r] * v[3];
            for (int r = 0; r < 4; ++r) c[r] = pr[r] * e[0] + pr[4 + r] * e[1] + pr[8 + r] * e[2] + pr[12 + r] * e[3];
            if (c[3] <= 0.0f) continue;
            float sx = (c[0] / c[3] * 0.5f + 0.5f) * S.viewport[2] + S.viewport[0];
            float sy = S.surf_h - ((c[1] / c[3] * 0.5f + 0.5f) * S.viewport[3] + S.viewport[1]);
            x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        }
        std::cout << std::endl << "[GLDump]     screen: x " << x0 << ".." << x1 << " y " << y0 << ".." << y1;
        std::cout << std::endl << "[GLDump]     model:";
        for (int c = 0; c < vs; ++c) {
            float mn = 1e9f, mx = -1e9f;
            for (uint32_t i = 0; i <= hi - lo; ++i) { mn = std::min(mn, pos[i * vs + c]); mx = std::max(mx, pos[i * vs + c]); }
            std::cout << " " << "xyzw"[c] << " " << mn << ".." << mx;
        }
        std::cout << std::endl;
    }
    std::vector<float> nrm, col, tex[2];
    if (CustomModel::Covers()) {   // Snake's skinned vertices: the custom player model instead
        float c[4] = {1, 1, 1, 1};
        if (S.color.enabled) {   // tint it with the game's lighting of this draw (its average vertex colour)
            col = GatherArray(uc, S.color, 4, lo, hi, true);
            const size_t n = col.size() / 4;
            for (int k = 0; k < 3 && n; ++k) { float t = 0; for (size_t i = 0; i < n; ++i) t += col[i * 4 + k]; c[k] = t / n; }
        } else glGetFloatv(GL_CURRENT_COLOR, c);
        CustomModel::Draw(c);
        return;
    }
    if (S.normal.enabled) nrm = GatherArray(uc, S.normal, 3, lo, hi, true);

    int vsize = S.vertex.size;
    const bool skinned = S.palette_enabled && S.mindex.enabled && S.weight.enabled;
    if (skinned) SkinVertices(uc, lo, hi, pos, vsize, S.normal.enabled ? &nrm : nullptr);

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(vsize, GL_FLOAT, 0, pos.data());

    if (S.normal.enabled) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, 0, nrm.data());
    } else glDisableClientState(GL_NORMAL_ARRAY);

    if (S.color.enabled) {
        col = GatherArray(uc, S.color, 4, lo, hi, true);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, 0, col.data());
    } else glDisableClientState(GL_COLOR_ARRAY);

    for (int u = 0; u < 2; ++u) {
        if (p_glClientActiveTexture) p_glClientActiveTexture(GL_TEXTURE0 + u);
        else if (u) break;
        if (S.texcoord[u].enabled) {
            tex[u] = GatherArray(uc, S.texcoord[u], S.texcoord[u].size, lo, hi, false);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(S.texcoord[u].size, GL_FLOAT, 0, tex[u].data());
        } else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (p_glClientActiveTexture) p_glClientActiveTexture(GL_TEXTURE0 + S.client_unit);

    if (skinned) { // palette output is already in eye space
        if (S.host_matrix_mode != GL_MODELVIEW) glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
    }
    // The game never sets a line width / point size: 1 px meant one guest pixel on the phone. Scale them to a
    // guest pixel in the host framebuffer, or lines (e.g. the codec tuning peaks) go hairline-thin at high res.
    const bool lines = (mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP), points = (mode == GL_POINTS);
    if (lines || points) {
        const float px = (std::max)(1.0f, (std::min)(S.GameScaleX(), S.GameScaleY()));
        if (lines) glLineWidth(px); else glPointSize(px);
        InfoOnce(std::string(lines ? "line" : "point") + " draw (mode " + std::to_string(mode) + "), width " + std::to_string(px));
    }
    if (AssetViewer::Recording()) {   // F1 > Models frame capture
        AssetViewer::DrawIn d;
        d.mode = mode; d.indices = &indices; d.count = count;
        d.pos = pos.data(); d.vsize = vsize; d.nverts = hi - lo + 1;
        if (S.texcoord[0].enabled) { d.uv = tex[0].data(); d.uvsize = S.texcoord[0].size; }
        if (S.color.enabled) d.col = col.data();
        d.eye_space = skinned;
        d.vptr = S.vertex.ptr;
        AssetViewer::AddDraw(d);
    }
    if (indices.empty()) glDrawArrays(mode, 0, count);
    else glDrawElements(mode, count, GL_UNSIGNED_SHORT, indices.data());
    if (lines) glLineWidth(1.0f);
    if (points) glPointSize(1.0f);
    if (skinned) {
        glPopMatrix();
        if (S.host_matrix_mode != GL_MODELVIEW) glMatrixMode(S.host_matrix_mode);
    }
}

// --- textures ---
int Components(uint32_t format) {
    switch (format) {
        case GL_ALPHA: case GL_LUMINANCE: return 1;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_RGB: return 3;
        default: return 4;
    }
}

size_t ImageSize(uint32_t w, uint32_t h, uint32_t format, uint32_t type) {
    size_t bpp = (type == GL_UNSIGNED_BYTE) ? Components(format) : 2;
    size_t row = w * bpp;
    size_t a = S.unpack_align;
    row = (row + a - 1) / a * a;
    return row * h;
}

void DecodePaletted(Cpu* uc, uint32_t target, int level, uint32_t fmt, uint32_t w, uint32_t h, uint32_t size, uint32_t data) {
    const int idx = fmt - GL_PALETTE4_RGB8_OES;   // 0..9
    const bool four = idx < 5;
    const int entries = four ? 16 : 256;
    const int kind = idx % 5;                     // 0 RGB8, 1 RGBA8, 2 R5G6B5, 3 RGBA4, 4 RGB5A1
    const int esz = kind == 0 ? 3 : kind == 1 ? 4 : 2;
    std::vector<uint8_t> raw = ReadMem(uc, data, size);
    uint32_t pal[256] = {};
    for (int i = 0; i < entries && (size_t)(i * esz + esz) <= raw.size(); ++i) {
        const uint8_t* e = &raw[i * esz];
        uint16_t s = e[0] | (esz > 1 ? e[1] << 8 : 0);
        uint8_t R, G, B, A = 255;
        switch (kind) {
            case 0: R = e[0]; G = e[1]; B = e[2]; break;
            case 1: R = e[0]; G = e[1]; B = e[2]; A = e[3]; break;
            case 2: R = ((s >> 11) & 31) * 255 / 31; G = ((s >> 5) & 63) * 255 / 63; B = (s & 31) * 255 / 31; break;
            case 3: R = ((s >> 12) & 15) * 17; G = ((s >> 8) & 15) * 17; B = ((s >> 4) & 15) * 17; A = (s & 15) * 17; break;
            default: R = ((s >> 11) & 31) * 255 / 31; G = ((s >> 6) & 31) * 255 / 31; B = ((s >> 1) & 31) * 255 / 31; A = (s & 1) ? 255 : 0; break;
        }
        pal[i] = R | (G << 8) | (B << 16) | (A << 24);
    }
    size_t off = static_cast<size_t>(entries) * esz;
    int levels = level <= 0 ? -level + 1 : 1;
#ifdef MGS_VITA
    if (level > 0) return;
    levels = 1;   // (no mip levels: G_TexImage2D)
#endif
    for (int l = 0; l < levels; ++l) {
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < px.size(); ++i) {
            int ix;
            if (four) {
                size_t b = off + i / 2;
                uint8_t v = b < raw.size() ? raw[b] : 0;
                ix = (i & 1) ? (v & 15) : (v >> 4);
            } else {
                size_t b = off + i;
                ix = b < raw.size() ? raw[b] : 0;
            }
            px[i] = pal[ix];
        }
        off += four ? (px.size() + 1) / 2 : px.size();
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(target, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, S.unpack_align);
        w = std::max<decltype(w)>(1, w / 2); h = std::max<decltype(h)>(1, h / 2);
    }
}

// Custom textures (README.md §6.19). runtime.cpp hooks the CIwTexture upload function (0x4A0607B0..0x4A060E28,
// r0 = the texture) and passes its hash here; its glTexImage2D calls (levels 0..n, 16-bit RGBA) are recognised by
// the return address. A replacement is uploaded as RGBA8 at level 0 with GL-generated mipmaps, and the game's own
// mip levels for that texture are then dropped.
std::atomic<uint32_t> g_upload_hash{0};
GLuint g_replaced_tex = 0;

bool ReplaceTexture(Cpu* uc, int level) {
    uint32_t lr = 0;
    uc->RegRead(Cpu::REG_LR, &lr);
    if (lr < G(0x4A0607B0) || lr >= G(0x4A060E28)) return false;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (level > 0) return g_replaced_tex && static_cast<GLuint>(bound) == g_replaced_tex;
    g_replaced_tex = 0;
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (!TexturePack::Replacement(g_upload_hash, px, w, h)) return false;
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, S.unpack_align);
    g_replaced_tex = static_cast<GLuint>(bound);
    AssetViewer::OnTexImage(static_cast<uint32_t>(bound), w, h, "RGBA8 (custom PNG)", g_upload_hash, true);
    CustomModel::NoteTexture(static_cast<uint32_t>(bound), TexturePack::OriginalPath(g_upload_hash));
    if (VerboseLog()) std::cout << "[Textures] Replaced " << std::hex << g_upload_hash << std::dec << " (" << w << "x" << h << ")" << std::endl;
    return true;
}

// F1 > Textures: level 0 of the bound texture was defined (named by its CIwTexture hash when it came through the
// CIwTexture upload).
void NoteTexture(Cpu* uc, uint32_t w, uint32_t h, const char* format) {
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    uint32_t lr = 0;
    uc->RegRead(Cpu::REG_LR, &lr);
    const bool iw = lr >= G(0x4A0607B0) && lr < G(0x4A060E28);
    CustomModel::NoteTexture(static_cast<uint32_t>(bound), iw ? TexturePack::OriginalPath(g_upload_hash) : std::string());
    AssetViewer::OnTexImage(static_cast<uint32_t>(bound), static_cast<int>(w), static_cast<int>(h), format, iw ? g_upload_hash.load() : 0, false);
}

const char* FormatName(uint32_t fmt, uint32_t type) {
    switch (type) {
        case GL_UNSIGNED_SHORT_5_5_5_1: return "RGBA 5551";
        case GL_UNSIGNED_SHORT_4_4_4_4: return "RGBA 4444";
        case GL_UNSIGNED_SHORT_5_6_5: return "RGB 565";
        default: break;
    }
    switch (fmt) {
        case GL_RGBA: return "RGBA8";
        case GL_RGB: return "RGB8";
        case GL_ALPHA: return "Alpha8";
        case GL_LUMINANCE: return "Luminance8";
        case GL_LUMINANCE_ALPHA: return "Luminance-alpha8";
        default: return "?";
    }
}

enum Fn {
    F_UNKNOWN,
    E_GetDisplay, E_Initialize, E_GetConfigs, E_ChooseConfig, E_GetConfigAttrib, E_CreateWindowSurface,
    E_CreateContext, E_MakeCurrent, E_SwapBuffers, E_DestroyContext, E_DestroySurface, E_Terminate,
    E_QueryString, E_GetProcAddress,
    G_ActiveTexture, G_AlphaFuncx, G_BindTexture, G_BlendFunc, G_Clear, G_ClearColor, G_ClearColorx,
    G_ClientActiveTexture, G_Color4x, G_ColorPointer, G_CompressedTexImage2D, G_CullFace,
    G_CurrentPaletteMatrixOES, G_DeleteTextures, G_DepthFunc, G_DepthMask, G_DepthRangef, G_DepthRangex,
    G_Disable, G_DisableClientState, G_DrawArrays, G_DrawElements, G_Enable, G_EnableClientState,
    G_Fogx, G_Fogxv, G_Frustumx, G_GenTextures, G_GetError, G_GetIntegerv, G_GetString, G_Hint,
    G_LightModelx, G_LightModelxv, G_Lightxv, G_LoadIdentity, G_LoadMatrixx, G_Materialxv,
    G_MatrixIndexPointerOES, G_MatrixMode, G_NormalPointer, G_Orthox, G_PixelStorei, G_PopMatrix,
    G_PushMatrix, G_Rotatex, G_Scalex, G_Scissor, G_ShadeModel, G_TexCoordPointer, G_TexEnvx,
    G_TexImage2D, G_TexParameterx, G_TexSubImage2D, G_Translatex, G_VertexPointer, G_Viewport,
    G_WeightPointerOES,
};

const std::unordered_map<std::string, Fn>& FnTable() {
    static const std::unordered_map<std::string, Fn> t = {
        {"eglGetDisplay", E_GetDisplay}, {"eglInitialize", E_Initialize}, {"eglGetConfigs", E_GetConfigs},
        {"eglChooseConfig", E_ChooseConfig}, {"eglGetConfigAttrib", E_GetConfigAttrib},
        {"eglCreateWindowSurface", E_CreateWindowSurface}, {"eglCreateContext", E_CreateContext},
        {"eglMakeCurrent", E_MakeCurrent}, {"eglSwapBuffers", E_SwapBuffers}, {"eglDestroyContext", E_DestroyContext},
        {"eglDestroySurface", E_DestroySurface}, {"eglTerminate", E_Terminate}, {"eglQueryString", E_QueryString},
        {"eglGetProcAddress", E_GetProcAddress},
        {"glActiveTexture", G_ActiveTexture}, {"glAlphaFuncx", G_AlphaFuncx}, {"glBindTexture", G_BindTexture},
        {"glBlendFunc", G_BlendFunc}, {"glClear", G_Clear}, {"glClearColor", G_ClearColor}, {"glClearColorx", G_ClearColorx},
        {"glClientActiveTexture", G_ClientActiveTexture}, {"glColor4x", G_Color4x}, {"glColorPointer", G_ColorPointer},
        {"glCompressedTexImage2D", G_CompressedTexImage2D}, {"glCullFace", G_CullFace},
        {"glCurrentPaletteMatrixOES", G_CurrentPaletteMatrixOES}, {"glDeleteTextures", G_DeleteTextures},
        {"glDepthFunc", G_DepthFunc}, {"glDepthMask", G_DepthMask}, {"glDepthRangef", G_DepthRangef},
        {"glDepthRangex", G_DepthRangex}, {"glDisable", G_Disable}, {"glDisableClientState", G_DisableClientState},
        {"glDrawArrays", G_DrawArrays}, {"glDrawElements", G_DrawElements}, {"glEnable", G_Enable},
        {"glEnableClientState", G_EnableClientState}, {"glFogx", G_Fogx}, {"glFogxv", G_Fogxv}, {"glFrustumx", G_Frustumx},
        {"glGenTextures", G_GenTextures}, {"glGetError", G_GetError}, {"glGetIntegerv", G_GetIntegerv},
        {"glGetString", G_GetString}, {"glHint", G_Hint}, {"glLightModelx", G_LightModelx},
        {"glLightModelxv", G_LightModelxv}, {"glLightxv", G_Lightxv}, {"glLoadIdentity", G_LoadIdentity},
        {"glLoadMatrixx", G_LoadMatrixx}, {"glMaterialxv", G_Materialxv},
        {"glMatrixIndexPointerOES", G_MatrixIndexPointerOES}, {"glMatrixMode", G_MatrixMode},
        {"glNormalPointer", G_NormalPointer}, {"glOrthox", G_Orthox}, {"glPixelStorei", G_PixelStorei},
        {"glPopMatrix", G_PopMatrix}, {"glPushMatrix", G_PushMatrix}, {"glRotatex", G_Rotatex}, {"glScalex", G_Scalex},
        {"glScissor", G_Scissor}, {"glShadeModel", G_ShadeModel}, {"glTexCoordPointer", G_TexCoordPointer},
        {"glTexEnvx", G_TexEnvx}, {"glTexImage2D", G_TexImage2D}, {"glTexParameterx", G_TexParameterx},
        {"glTexSubImage2D", G_TexSubImage2D}, {"glTranslatex", G_Translatex}, {"glVertexPointer", G_VertexPointer},
        {"glViewport", G_Viewport}, {"glWeightPointerOES", G_WeightPointerOES},
    };
    return t;
}

bool IsEsOnlyCap(uint32_t cap) { return cap == GL_MATRIX_PALETTE_OES; }

// Render target size: the guest screen itself (native resolution) or the output size x supersampling
void SetRenderSize() {
    if (S.native) { S.fbo_w = S.surf_w; S.fbo_h = S.surf_h; }
    else { S.fbo_w = S.out_w * S.ss; S.fbo_h = S.out_h * S.ss; }
}

// Guest surface coordinates -> host framebuffer pixels
void ApplyViewport() {   // the guest's viewport / scissor, into the game's box of the render target
    const float sx = S.GameScaleX(), sy = S.GameScaleY();
    const int ox = S.GameX(), oy = S.GameY();
    auto px = [](int v, float s) { return static_cast<GLint>(std::lround(v * s)); };
    glViewport(ox + px(S.viewport[0], sx), oy + px(S.viewport[1], sy), px(S.viewport[2], sx), px(S.viewport[3], sy));
    glScissor(ox + px(S.scissor[0], sx), oy + px(S.scissor[1], sy), px(S.scissor[2], sx), px(S.scissor[3], sy));
}

// Black bars around the game's box in the bound output (the game's clears may reach them; the overlay draws over
// them every frame).
void ClearBars(int w, int h, int gx, int gy, int gw, int gh) {
    if (gx <= 0 && gy <= 0 && gw >= w && gh >= h) return;
    GLStateSave save;
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);   // (not in GLStateSave on ES)
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const int rects[4][4] = { {0, 0, gx, h}, {gx + gw, 0, w - gx - gw, h}, {gx, 0, gw, gy}, {gx, gy + gh, gw, h - gy - gh} };
    for (const auto& r : rects) {
        if (r[2] <= 0 || r[3] <= 0) continue;
        glScissor(r[0], r[1], r[2], r[3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
}

} // namespace

GLES& GLES::Instance() {
    static GLES g;
    return g;
}

void GLES::SetHost(Host host) { g_host = std::move(host); }
void* GLES::Proc(const char* name) { return g_host.get_proc ? g_host.get_proc(name) : nullptr; }

uint32_t GLES::GuestString(const std::string& s) {
    auto it = S.strings.find(s);
    if (it != S.strings.end()) return it->second;
    uint32_t a = alloc_ ? alloc_(static_cast<uint32_t>(s.size() + 1)) : 0;
    // uc handle is not stored here; strings are written by the caller via Handle (see below)
    S.strings[s] = a;
    return a;
}

bool GLES::EnsureContext() {
    if (S.ok) return true;
    if (S.tried) return false;   // an earlier attempt failed
    S.tried = true;
    if (!g_host.make_current || !g_host.make_current()) { WarnOnce("no GL context (the platform layer could not make one current)"); return false; }

    p_glActiveTexture          = LoadGL<PFN_ActiveTexture>("glActiveTexture", "glActiveTextureARB");
    p_glClientActiveTexture    = LoadGL<PFN_ActiveTexture>("glClientActiveTexture", "glClientActiveTextureARB");
    p_glGenFramebuffers        = LoadGL<PFN_GenFramebuffers>("glGenFramebuffers", "glGenFramebuffersEXT", "glGenFramebuffersOES");
    p_glBindFramebuffer        = LoadGL<PFN_BindFramebuffer>("glBindFramebuffer", "glBindFramebufferEXT", "glBindFramebufferOES");
    p_glGenRenderbuffers       = LoadGL<PFN_GenRenderbuffers>("glGenRenderbuffers", "glGenRenderbuffersEXT", "glGenRenderbuffersOES");
    p_glBindRenderbuffer       = LoadGL<PFN_BindRenderbuffer>("glBindRenderbuffer", "glBindRenderbufferEXT", "glBindRenderbufferOES");
    p_glRenderbufferStorage    = LoadGL<PFN_RenderbufferStorage>("glRenderbufferStorage", "glRenderbufferStorageEXT", "glRenderbufferStorageOES");
    p_glFramebufferRenderbuffer = LoadGL<PFN_FramebufferRenderbuffer>("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT", "glFramebufferRenderbufferOES");
    p_glCheckFramebufferStatus = LoadGL<PFN_CheckFramebufferStatus>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT", "glCheckFramebufferStatusOES");
#if !defined(MGS_GLES) || defined(MGS_VITA)   // (ES 1.1: no blits, so no MSAA, supersampling or native resolution yet; vitaGL has them)
    p_glRenderbufferStorageMultisample = LoadGL<PFN_RenderbufferStorageMultisample>("glRenderbufferStorageMultisample", "glRenderbufferStorageMultisampleEXT");
    p_glBlitFramebuffer        = LoadGL<PFN_BlitFramebuffer>("glBlitFramebuffer", "glBlitFramebufferEXT");
#endif
#ifdef MGS_VITA
    p_glFramebufferTexture2D   = LoadGL<PFN_FramebufferTexture2D>("glFramebufferTexture2D");
#endif
    if (!p_glGenFramebuffers || !p_glBindFramebuffer || !p_glGenRenderbuffers || !p_glRenderbufferStorage) {
        WarnOnce("framebuffer objects unavailable");
        return false;
    }

    if (!CreateFramebuffer()) return false;
    std::cout << "[GLES] Host context: " << glGetString(GL_VENDOR) << " / " << glGetString(GL_RENDERER)
              << " / " << glGetString(GL_VERSION) << std::endl;
    S.ok = true;
    return true;
}

// Render target + (when anti-aliasing is on) the resolve/output targets. The game renders into S.fbo at
// output size x supersampling factor, multisampled with MSAA; Present() resolves / downsamples into S.out_fbo.
bool GLES::CreateFramebuffer() {
    aa_applied_msaa_ = aa_msaa_; aa_applied_ss_ = aa_ss_; applied_native_ = native_res_;
    GLint max_samples = 0, max_size = 4096;
    if (p_glRenderbufferStorageMultisample && p_glBlitFramebuffer) glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    S.msaa = p_glBlitFramebuffer ? (std::min)(aa_applied_msaa_, static_cast<int>(max_samples)) : 0;
    if (S.msaa < 2) S.msaa = 0;
    S.native = p_glBlitFramebuffer && applied_native_;
    S.ss = (p_glBlitFramebuffer && !S.native) ? (std::max)(1, aa_applied_ss_) : 1;
    while (S.ss > 1 && (S.out_w * S.ss > max_size || S.out_h * S.ss > max_size)) --S.ss;
    SetRenderSize();

    auto storage = [](GLuint rb, int samples, GLenum fmt, int w, int h) {
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        if (samples > 0) p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
        else p_glRenderbufferStorage(GL_RENDERBUFFER, fmt, w, h);
    };
    auto complete = [](const char* what) {
        GLenum st = p_glCheckFramebufferStatus ? p_glCheckFramebufferStatus(GL_FRAMEBUFFER) : GL_FRAMEBUFFER_COMPLETE;
        if (st != GL_FRAMEBUFFER_COMPLETE) std::cout << "[GLES] " << what << " FBO incomplete 0x" << std::hex << st << std::dec << std::endl;
        return st == GL_FRAMEBUFFER_COMPLETE;
    };
    bool ok = true;
    if (S.msaa > 0 || S.Scaled()) {   // window-size output
        if (!S.out_fbo) { p_glGenFramebuffers(1, &S.out_fbo); p_glGenRenderbuffers(1, &S.rb_out); }
        p_glBindFramebuffer(GL_FRAMEBUFFER, S.out_fbo);
        storage(S.rb_out, 0, GL_RGBA8, S.out_w, S.out_h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, S.rb_out);
        ok = complete("output");
    }
    if (ok && S.msaa > 0 && S.Scaled()) {   // MSAA resolves at full render size before the rescale
        if (!S.resolve_fbo) { p_glGenFramebuffers(1, &S.resolve_fbo); p_glGenRenderbuffers(1, &S.rb_resolve); }
        p_glBindFramebuffer(GL_FRAMEBUFFER, S.resolve_fbo);
        storage(S.rb_resolve, 0, GL_RGBA8, S.fbo_w, S.fbo_h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, S.rb_resolve);
        ok = complete("resolve");
    }
    if (ok) {
        if (!S.fbo) p_glGenFramebuffers(1, &S.fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, S.fbo);
#ifdef MGS_VITA   // vitaGL: a colour renderbuffer leaves the framebuffer incomplete (0x8CD7); a texture works
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        if (!S.tex_color) glGenTextures(1, &S.tex_color);
        glBindTexture(GL_TEXTURE_2D, S.tex_color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S.fbo_w, S.fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, S.tex_color, 0);
#else
        if (!S.rb_color) p_glGenRenderbuffers(1, &S.rb_color);
        storage(S.rb_color, S.msaa, GL_RGBA8, S.fbo_w, S.fbo_h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, S.rb_color);
#endif
        if (!S.rb_depth) p_glGenRenderbuffers(1, &S.rb_depth);
        storage(S.rb_depth, S.msaa, GL_DEPTH24_STENCIL8, S.fbo_w, S.fbo_h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, S.rb_depth);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, S.rb_depth);
        ok = complete("render");
#ifdef MGS_GLES
        if (!ok) {   // the ES 1.1 baseline: RGB565 colour, 16-bit depth, no stencil
            std::cout << "[GLES] Falling back to an RGB565 / 16-bit depth render target" << std::endl;
            storage(S.rb_color, 0, 0x8D62 /*GL_RGB565_OES*/, S.fbo_w, S.fbo_h);
            p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            storage(S.rb_depth, 0, 0x81A5 /*GL_DEPTH_COMPONENT16_OES*/, S.fbo_w, S.fbo_h);
            p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, S.rb_depth);
            ok = complete("render (RGB565)");
        }
#endif
    }
    if (!ok) {
        if (S.msaa || S.Scaled()) {   // fall back to no anti-aliasing / native resolution
            std::cout << "[GLES] Anti-aliasing / native resolution unavailable at this size, turning it off" << std::endl;
            aa_msaa_ = 0; aa_ss_ = 1; native_res_ = false;
            return CreateFramebuffer();
        }
        return false;
    }
    ApplyViewport();
    std::cout << "[GLES] Render target " << S.fbo_w << "x" << S.fbo_h << " (MSAA " << S.msaa << "x, supersampling "
              << S.ss << "x, " << (S.native ? "native resolution, " : "") << "output " << S.out_w << "x" << S.out_h << ") for a " << S.surf_w << "x" << S.surf_h << " screen" << std::endl;
    return true;
}

// Present: MSAA resolve and/or supersampling downsample into the output target, which is then bound for the
// overlay and the readback. BindRenderTarget() switches back to the game's target.
void GLES::ResolveFrame() {
    if (!(S.msaa > 0 || S.Scaled())) return;
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);   // blits are clipped by the scissor test
    GLuint src = S.fbo;
    if (S.msaa > 0 && S.Scaled()) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, S.fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, S.resolve_fbo);
        p_glBlitFramebuffer(0, 0, S.fbo_w, S.fbo_h, 0, 0, S.fbo_w, S.fbo_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        src = S.resolve_fbo;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, S.out_fbo);
    if (S.native)   // the game alone, into its box
        p_glBlitFramebuffer(0, 0, S.fbo_w, S.fbo_h, S.game_x, S.game_y, S.game_x + S.game_w, S.game_y + S.game_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    else
        p_glBlitFramebuffer(0, 0, S.fbo_w, S.fbo_h, 0, 0, S.out_w, S.out_h, GL_COLOR_BUFFER_BIT, S.ss > 1 ? GL_LINEAR : GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, S.out_fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

// Direct presentation: the finished frame (S.out_fbo with AA, else S.fbo) is blitted to the window's framebuffer,
// aspect-fitted (it is the window's size unless the window is past the 4096 cap), and swapped. ES 1.1 has no
// blits: the game target is then the window itself (not done yet, README Port Roadmap).
void GLES::PresentToWindow(int W, int H) {
    int dw = 0, dh = 0;
    g_host.window_size(dw, dh);
    if (dw <= 0 || dh <= 0 || !p_glBlitFramebuffer) { g_host.swap(); return; }
    const GLuint src = (S.msaa > 0 || S.Scaled()) ? S.out_fbo : S.fbo;
    const double s = (std::min)(double(dw) / W, double(dh) / H);
    const int w = static_cast<int>(W * s + 0.5), h = static_cast<int>(H * s + 0.5), x = (dw - w) / 2, y = (dh - h) / 2;
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (w != dw || h != dh) { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBlitFramebuffer(0, 0, W, H, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, (w == W && h == H) ? GL_NEAREST : GL_LINEAR);
    {   // debug: MGS_WINDOW_SHOT_AT=<n> saves what the n-th presented frame put in the window (window.png in the
        // working directory), read from the window's own framebuffer before the swap
        static const int at = getenv("MGS_WINDOW_SHOT_AT") ? atoi(getenv("MGS_WINDOW_SHOT_AT")) : 0;
        static int n = 0;
        if (at && ++n == at) {
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            std::vector<uint32_t> px(static_cast<size_t>(dw) * dh), flip(px.size());
            ReadPixelsBGRA(dw, dh, px.data());
            for (int r = 0; r < dh; ++r) memcpy(&flip[static_cast<size_t>(r) * dw], &px[static_cast<size_t>(dh - 1 - r) * dw], static_cast<size_t>(dw) * 4);
            for (uint32_t& p : flip) p |= 0xFF000000u;
            if (shot_cb_) shot_cb_(std::move(flip), dw, dh);   // saved like an F12 screenshot
        }
    }
    g_host.swap();
    p_glBindFramebuffer(GL_FRAMEBUFFER, src);   // the overlay pass and the readback read this one
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void GLES::BindRenderTarget() {
    if (S.msaa > 0 || S.Scaled()) p_glBindFramebuffer(GL_FRAMEBUFFER, S.fbo);
}

void GLES::SetDisplay(int surf_w, int surf_h, int game_w, int game_h, int out_w, int out_h) {
    out_w = (std::max)(out_w, game_w); out_h = (std::max)(out_h, game_h);
    if (surf_w == S.surf_w && surf_h == S.surf_h && game_w == S.game_w && game_h == S.game_h && out_w == S.out_w && out_h == S.out_h) return;
    const bool rotated = (surf_w != S.surf_w || surf_h != S.surf_h);
    S.surf_w = surf_w; S.surf_h = surf_h; S.out_w = out_w; S.out_h = out_h;
    S.game_w = game_w; S.game_h = game_h; S.game_x = (out_w - game_w) / 2; S.game_y = (out_h - game_h) / 2;
    SetRenderSize();
    if (rotated) { // the game re-establishes its viewport after a screen change; start from full screen
        S.viewport[0] = S.viewport[1] = S.scissor[0] = S.scissor[1] = 0;
        S.viewport[2] = S.scissor[2] = surf_w; S.viewport[3] = S.scissor[3] = surf_h;
    }
    if (S.ok) CreateFramebuffer();
}

// The finished game frame before the overlay: the full-size single-sample image (S.fbo without MSAA,
// resolve_fbo with MSAA + supersampling, out_fbo with MSAA only), read bottom-up and flipped.
void GLES::CaptureScreenshot() {
    if (!shot_cb_) return;
    GLuint src = S.fbo;
    int w = S.fbo_w, h = S.fbo_h;
    if (S.msaa > 0) {
        if (S.Scaled()) src = S.resolve_fbo;
        else { src = S.out_fbo; w = S.out_w; h = S.out_h; }
    }
    std::vector<uint32_t> px(static_cast<size_t>(w) * h), out(px.size());
    if (p_glBlitFramebuffer) {   // separate read binding (with blits); without them src is S.fbo, already bound
        GLint prev = 0;
        glGetIntegerv(0x8CAA /*GL_READ_FRAMEBUFFER_BINDING*/, &prev);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        ReadPixelsBGRA(w, h, px.data());
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev));
    } else {
        ReadPixelsBGRA(w, h, px.data());
    }
    // Only the game's box (not the bars around it)
    int bx = S.GameX(), by = S.GameY(), bw = std::lround(S.GameScaleX() * S.surf_w), bh = std::lround(S.GameScaleY() * S.surf_h);
    if (src == S.out_fbo) { bx = S.game_x; by = S.game_y; bw = S.game_w; bh = S.game_h; }
    bw = (std::min)(bw, w - bx); bh = (std::min)(bh, h - by);
    out.resize(static_cast<size_t>(bw) * bh);
    for (int y = 0; y < bh; ++y)
        memcpy(&out[static_cast<size_t>(y) * bw], &px[static_cast<size_t>(by + bh - 1 - y) * w + bx], static_cast<size_t>(bw) * 4);
    shot_cb_(std::move(out), bw, bh);
}

void GLES::SetHide2D(bool hide) { g_hide_2d = hide; }
void GLES::SetUploadTexture(uint32_t hash) { g_upload_hash = hash; }

void GLES::SetWarningCallback(std::function<void(const std::string&)> cb) { g_warning_cb = std::move(cb); }

void GLES::Present() {
    if (!S.ok || !frame_cb_) return;
    AssetViewer::OnPresentBegin();   // a frame capture for F1 > Models ends here
    {   // debug: MGS_GLDUMP_AT=<n> logs the GL calls of the n-th presented frame ([GLDump], like Shift+F12)
        static const int dump_at = getenv("MGS_GLDUMP_AT") ? atoi(getenv("MGS_GLDUMP_AT")) : 0;
        static int presented = 0;
        if (dump_at && ++presented == dump_at) dump_requested_ = true;
    }
    // Frame pacing: target ~60 FPS (16.666 ms per frame)
    static auto last_present = std::chrono::steady_clock::time_point{};
    auto pace = [] {
        auto now = std::chrono::steady_clock::now();
        if (last_present.time_since_epoch().count() != 0) {
            constexpr auto frame_target = std::chrono::microseconds(16666);
            auto elapsed = now - last_present;
            if (elapsed < frame_target) {
                std::this_thread::sleep_for(frame_target - elapsed);
                now = std::chrono::steady_clock::now();
            }
        }
        last_present = now;
    };
    pace();
    ResolveFrame();
    ClearBars(S.out_w, S.out_h, S.game_x, S.game_y, S.game_w, S.game_h);
    if (shot_requested_ && !(paused_ && paused_())) { shot_requested_ = false; CaptureScreenshot(); }
    const int W = S.out_w, H = S.out_h;
    static std::vector<uint32_t> px, flipped;
    px.resize(static_cast<size_t>(W) * H);
    flipped.resize(px.size());
    auto readback = [&] {
        if (g_host.swap) { PresentToWindow(W, H); if (!cpu_frames_) { frame_cb_(nullptr, W, H); return; } }
        const auto t0 = std::chrono::steady_clock::now();
        ReadPixelsBGRA(W, H, px.data());
        const auto t1 = std::chrono::steady_clock::now();
        for (int y = 0; y < H; ++y) memcpy(&flipped[static_cast<size_t>(y) * W], &px[static_cast<size_t>(H - 1 - y) * W], W * 4);
        frame_cb_(flipped.data(), W, H);
        {   // debug: MGS_FRAME_TIMES=1 logs the average read-back and frame-to-frame times every 120 frames
            static const bool on = getenv("MGS_FRAME_TIMES") != nullptr;
            static double rb = 0, total = 0;
            static int n = 0;
            static auto prev = t0;
            if (on) {
                rb += std::chrono::duration<double, std::milli>(t1 - t0).count();
                total += std::chrono::duration<double, std::milli>(t0 - prev).count();
                prev = t0;
                if (++n == 120) {
                    std::cout << "[Frame] " << W << "x" << H << ": read-back " << rb / n << " ms, frame " << total / n << " ms" << std::endl;
                    rb = total = 0; n = 0;
                }
            }
        }
    };

    // F1 overlay open: the game is frozen here (this thread doesn't return to it) while the overlay keeps
    // being redrawn over the game's last frame; the paused time is then reported so the game clock skips it.
    const bool pause = paused_ && paused_();
    static GLuint frozen_tex = 0;   // the game frame while paused (a texture: ES has no glDrawPixels)
#ifdef MGS_3DS
    // 3DS: the overlay is on the bottom screen and the top screen keeps the last frame by itself (a frame with no
    // draws for the top screen leaves it as it is), so no copy of the frame is needed.
    if (pause) SavePausedState();
#else
    if (pause) {
        SavePausedState();
        GLStateSave save;
        if (!frozen_tex) glGenTextures(1, &frozen_tex);
        glBindTexture(GL_TEXTURE_2D, frozen_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, W, H, 0);   // (not a power of two: GL 2+ / ES NPOT)
    }
#endif
    if (overlay_cb_) overlay_cb_(W, H);
    readback();
    ++S.frames;
    auto finish = [&] {   // back to the game's target; anti-aliasing changes take effect between frames
        AssetViewer::OnPresentEnd();
        if (aa_msaa_ != aa_applied_msaa_ || aa_ss_ != aa_applied_ss_ || native_res_ != applied_native_) CreateFramebuffer();
        else BindRenderTarget();
    };
    if (!pause) { finish(); return; }

    const auto t0 = std::chrono::steady_clock::now();
    while (paused_()) {
        if (background_) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }   // nobody sees it
        pace();
#ifndef MGS_3DS
        {   // put the game frame back (a plain copy, all state saved), then the overlay on top
            GLStateSave save;
            glViewport(0, 0, W, H);
            GLDrawFullscreenTexture(frozen_tex, false);
        }
#endif
        if (overlay_cb_) overlay_cb_(W, H);
        readback();
    }
    if (resumed_) resumed_(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    finish();
}

void GLES::SavePausedState() {
    GuestState& g = paused_state_;
    g.valid = true;
    g.matrix_mode = S.matrix_mode;
    glGetFloatv(GL_MODELVIEW_MATRIX, g.mv);
    glGetFloatv(GL_PROJECTION_MATRIX, g.proj);
    GLint active = GL_TEXTURE0, client = GL_TEXTURE0;
    glGetIntegerv(0x84E0 /*GL_ACTIVE_TEXTURE*/, &active);
    glGetIntegerv(0x84E1 /*GL_CLIENT_ACTIVE_TEXTURE*/, &client);
    g.active_unit = active - GL_TEXTURE0;
    g.client_unit = S.client_unit;
    for (int u = 0; u < 2; ++u) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + u);
        else if (u) break;
        g.tex2d[u] = glIsEnabled(GL_TEXTURE_2D) != 0;
        GLint b = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &b);
        g.bound[u] = static_cast<uint32_t>(b);
        if (u == 0) glGetFloatv(GL_TEXTURE_MATRIX, g.tex);
    }
    if (p_glActiveTexture) p_glActiveTexture(static_cast<GLenum>(active));
    g.blend = glIsEnabled(GL_BLEND) != 0;
    g.alpha_test = glIsEnabled(GL_ALPHA_TEST) != 0;
    glGetFloatv(GL_CURRENT_COLOR, g.color4);
    auto arr = [](const ClientArray& a) { return GuestState::Array{a.enabled, a.size, a.type, a.stride, a.ptr}; };
    g.vertex = arr(S.vertex); g.color = arr(S.color); g.texcoord[0] = arr(S.texcoord[0]); g.texcoord[1] = arr(S.texcoord[1]);
    g.unpack_align = S.unpack_align;
}

// Software renderer: IwGx rasterised the frame into the s3eSurface (RGB565, surf_w x surf_h). It is scaled up
// into the render target as a textured quad (nearest unless texture filtering is on), then presented like a
// GL frame, so pacing, the overlay, pausing and screenshots work the same. The game makes no GL calls in this
// mode, so the state is ours, but it is saved and restored anyway.
bool GLES::PresentSoftware(const uint16_t* rgb565, int w, int h) {
    if (!frame_cb_ || !EnsureContext()) return false;
    static GLuint tex = 0;
    if (!tex) glGenTextures(1, &tex);
    {
    GLStateSave save;
    p_glBindFramebuffer(GL_FRAMEBUFFER, S.fbo);
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint filter = filter_wanted_ ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, 0x8363 /*GL_UNSIGNED_SHORT_5_6_5*/, rgb565);
    glViewport(S.GameX(), S.GameY(), std::lround(S.GameScaleX() * S.surf_w), std::lround(S.GameScaleY() * S.surf_h));
    GLDrawFullscreenTexture(tex, true);   // surface row 0 is the top of the screen
    }
    Present();
    return true;
}

bool GLES::Handle(const std::string& name, Cpu* uc, const uint32_t r[4], uint32_t& ret) {
    // name lives in the runtime's import list for the whole run: cache the lookup by its address (hashing the
    // name on each of the ~2000 GL calls a frame cost several ms on the 3DS).
    struct Slot { const std::string* key; int fn; };
    static Slot cache[512];   // direct-mapped on the name's address (no hashing, no division)
    Slot& c = cache[(reinterpret_cast<uintptr_t>(&name) / sizeof(std::string)) & 511];
    if (c.key != &name) {
        const auto& tbl = FnTable();
        auto it = tbl.find(name);
        c = {&name, it == tbl.end() ? -1 : static_cast<int>(it->second)};
    }
    if (c.fn < 0) return false;
    const Fn fn = static_cast<Fn>(c.fn);
    ret = 0;
    if (filter_wanted_ != filter_applied_) ApplyFilterMode();
    if (dumping_) {
        uint32_t lr = 0;
        uc->RegRead(Cpu::REG_LR, &lr);
        std::cout << "[GLDump] " << name << std::hex << " 0x" << r[0] << " 0x" << r[1] << " 0x" << r[2] << " 0x" << r[3]
                  << " lr=0x" << lr;
        if (fn == G_LoadMatrixx) { // matrix values + rough Thumb backtrace (return addresses found on the stack)
            int32_t m[16] = {};
            uc->MemRead(r[0], m, sizeof(m));
            std::cout << std::dec << " m=";
            for (int i = 0; i < 16; ++i) std::cout << (i ? "," : "") << m[i] / 65536.0f;
            uint32_t sp = 0, words[96] = {};
            uc->RegRead(Cpu::REG_SP, &sp);
            uc->MemRead(sp, words, sizeof(words));
            std::cout << std::hex << " bt:";
            for (uint32_t w : words) if ((w & 1) && w >= G(0x4A000000) && w < G(0x4A0F2000)) std::cout << " " << w;
        }
        std::cout << std::dec << std::endl;
    }
    g_dump_draw = dumping_;
    auto A = [&](int i) { return Arg(uc, r, i); };
    auto W32 = [&](uint32_t addr, uint32_t v) { if (addr) uc->MemWrite(addr, &v, 4); };
    auto Str = [&](const std::string& s) {
        bool fresh = S.strings.find(s) == S.strings.end();
        uint32_t a = GuestString(s);
        if (fresh && a) uc->MemWrite(a, s.c_str(), s.size() + 1);
        return a;
    };

    // ---- EGL ----
    switch (fn) {
        case E_GetDisplay: ret = 1; return true;
        case E_Initialize: W32(r[1], 1); W32(r[2], 1); ret = 1; return true;
        case E_GetConfigs:  // (dpy, configs, size, num)
            if (r[1] && r[2]) W32(r[1], 1);
            W32(r[3], 1); ret = 1; return true;
        case E_ChooseConfig: // (dpy, attribs, configs, size, num)
            if (r[2] && r[3]) W32(r[2], 1);
            W32(A(4), 1); ret = 1; return true;
        case E_GetConfigAttrib: { // (dpy, config, attr, value*)
            uint32_t v = 0;
            switch (r[2]) {
                case 0x3020: v = 16; break;          // BUFFER_SIZE
                case 0x3021: v = 0; break;           // ALPHA_SIZE
                case 0x3022: v = 5; break;           // BLUE_SIZE
                case 0x3023: v = 6; break;           // GREEN_SIZE
                case 0x3024: v = 5; break;           // RED_SIZE
                case 0x3025: v = 16; break;          // DEPTH_SIZE
                case 0x3026: v = 8; break;           // STENCIL_SIZE
                case 0x3027: v = 0x3038; break;      // CONFIG_CAVEAT = EGL_NONE
                case 0x3028: v = 1; break;           // CONFIG_ID
                case 0x3033: v = 0x7; break;         // SURFACE_TYPE = PBUFFER|PIXMAP|WINDOW
                case 0x303A: v = 0; break;           // TRANSPARENT_TYPE...
                case 0x3040: v = 1; break;           // RENDERABLE_TYPE = ES
                default: v = 0; break;
            }
            W32(r[3], v); ret = 1; return true;
        }
        case E_CreateWindowSurface: ret = EnsureContext() ? 1 : 0; return true;
        case E_CreateContext: ret = EnsureContext() ? 1 : 0; return true;
        case E_MakeCurrent: ret = 1; return true;
        case E_SwapBuffers:
            Present();
            if (dumping_) { dumping_ = false; std::cout << "[GLDump] ---- end of frame ----" << std::endl; }
            if (dump_requested_) { dump_requested_ = false; dumping_ = true; std::cout << "[GLDump] ---- frame " << S.frames << " ----" << std::endl; }
            ret = 1; return true;
        case E_DestroyContext: case E_DestroySurface: case E_Terminate: ret = 1; return true;
        case E_QueryString:
            switch (r[1]) {
                case 0x3053: ret = Str("Imagination Technologies"); break; // EGL_VENDOR
                case 0x3054: ret = Str("1.1"); break;                      // EGL_VERSION
                case 0x3055: ret = Str(""); break;                         // EGL_EXTENSIONS
                default: ret = Str(""); break;
            }
            return true;
        case E_GetProcAddress: {
            char nm[64] = {};
            uc->MemRead(r[0], nm, sizeof(nm) - 1);
            WarnOnce("eglGetProcAddress(" + std::string(nm) + ") -> 0");
            ret = 0;
            return true;
        }
        default: break;
    }

    // ---- GL (state-only calls are safe before a context exists, but the host needs one) ----
    if (!S.ok) {
        if (fn == G_GetError) { ret = 0; return true; }
        if (fn == G_GetString || fn == G_GetIntegerv) EnsureContext();
        else { WarnOnce(name + " before context"); return true; }
    }

    switch (fn) {
        case G_GetString:
            switch (r[0]) {
                case GL_VENDOR:     ret = Str("Imagination Technologies"); break;
                case GL_RENDERER:   ret = Str("PowerVR MBX VGP"); break;
                case GL_VERSION:    ret = Str("OpenGL ES-CM 1.1"); break;
                case GL_EXTENSIONS: ret = Str("GL_OES_byte_coordinates GL_OES_fixed_point GL_OES_single_precision "
                                              "GL_OES_read_format GL_OES_compressed_paletted_texture GL_OES_matrix_palette"); break;
                default: ret = 0; break;
            }
            return true;
        case G_GetError: ret = 0; return true;
        case G_GetIntegerv: {
            int v[4] = {0, 0, 0, 0}; int n = 1;
            switch (r[0]) {
                case GL_VIEWPORT: memcpy(v, S.viewport, sizeof(v)); n = 4; break;
                case GL_SCISSOR_BOX: memcpy(v, S.scissor, sizeof(v)); n = 4; break;
                case GL_MAX_TEXTURE_UNITS: v[0] = 2; break;
                case GL_MAX_TEXTURE_SIZE: v[0] = 1024; break;
                case GL_MAX_LIGHTS: v[0] = 8; break;
                case GL_MAX_PALETTE_MATRICES_OES: v[0] = State::kPaletteSize; break;
                case GL_MAX_VERTEX_UNITS_OES: v[0] = State::kVertexUnits; break;
                case GL_RED_BITS: case GL_BLUE_BITS: v[0] = 5; break;
                case GL_GREEN_BITS: v[0] = 6; break;
                case GL_ALPHA_BITS: v[0] = 0; break;
                case GL_DEPTH_BITS: v[0] = 16; break;
                case GL_STENCIL_BITS: v[0] = 8; break;
                case GL_NUM_COMPRESSED_TEXTURE_FORMATS: v[0] = 10; break;
                case GL_COMPRESSED_TEXTURE_FORMATS: {
                    for (uint32_t i = 0; i < 10; ++i) W32(r[1] + 4 * i, GL_PALETTE4_RGB8_OES + i);
                    return true;
                }
                case GL_IMPLEMENTATION_COLOR_READ_TYPE_OES: v[0] = GL_UNSIGNED_SHORT_5_6_5; break;
                case GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES: v[0] = GL_RGB; break;
                default: {
                    GLint hv[16] = {};
                    glGetIntegerv(r[0], hv);
                    v[0] = hv[0];
                    char b[64]; snprintf(b, sizeof b, "glGetIntegerv(0x%x) passed through", r[0]);
                    WarnOnce(b);
                }
            }
            for (int i = 0; i < n; ++i) W32(r[1] + 4 * i, static_cast<uint32_t>(v[i]));
            { char b[64]; snprintf(b, sizeof b, "glGetIntegerv(0x%x) -> %d", r[0], v[0]); InfoOnce(b); }
            return true;
        }

        // client arrays
        case G_VertexPointer:   S.vertex = {S.vertex.enabled, (int)r[0], r[1], r[2], r[3]}; return true;
        case G_ColorPointer:    S.color = {S.color.enabled, (int)r[0], r[1], r[2], r[3]}; return true;
        case G_NormalPointer:   S.normal = {S.normal.enabled, 3, r[0], r[1], r[2]}; return true;
        case G_TexCoordPointer: { auto& t = S.texcoord[S.client_unit & 1]; t = {t.enabled, (int)r[0], r[1], r[2], r[3]}; return true; }
        case G_EnableClientState: case G_DisableClientState: {
            bool on = fn == G_EnableClientState;
            switch (r[0]) {
                case GL_VERTEX_ARRAY: S.vertex.enabled = on; break;
                case GL_NORMAL_ARRAY: S.normal.enabled = on; break;
                case GL_COLOR_ARRAY: S.color.enabled = on; break;
                case GL_TEXTURE_COORD_ARRAY: S.texcoord[S.client_unit & 1].enabled = on; break;
                case GL_MATRIX_INDEX_ARRAY_OES: S.mindex.enabled = on; break;
                case GL_WEIGHT_ARRAY_OES: S.weight.enabled = on; break;
                default: WarnOnce("client state 0x" + std::to_string(r[0]) + " ignored"); break;
            }
            return true;
        }
        case G_ClientActiveTexture: S.client_unit = r[0] - GL_TEXTURE0; if (p_glClientActiveTexture) p_glClientActiveTexture(r[0]); return true;
        case G_ActiveTexture: if (p_glActiveTexture) p_glActiveTexture(r[0]); return true;
        case G_DrawArrays: Draw(uc, r[0], r[1], r[2], 0, 0); return true;
        case G_DrawElements: Draw(uc, r[0], 0, r[1], r[2] ? r[2] : GL_UNSIGNED_SHORT, r[3]); return true;

        // matrix palette (not advertised; log if the game uses it anyway)
        // GL_OES_matrix_palette
        case G_CurrentPaletteMatrixOES: S.cur_palette = std::min<int>(r[0], State::kPaletteSize - 1); return true;
        case G_MatrixIndexPointerOES: S.mindex = {S.mindex.enabled, (int)r[0], r[1], r[2], r[3]}; return true;
        case G_WeightPointerOES:      S.weight = {S.weight.enabled, (int)r[0], r[1], r[2], r[3]}; return true;

        // state
        case G_Enable:  if (!IsEsOnlyCap(r[0])) glEnable(r[0]); else S.palette_enabled = true; return true;
        case G_Disable: if (!IsEsOnlyCap(r[0])) glDisable(r[0]); else S.palette_enabled = false; return true;
        case G_AlphaFuncx: glAlphaFunc(r[0], Fx(r[1])); return true;
        case G_BlendFunc: glBlendFunc(r[0], r[1]); return true;
        case G_Clear: glClear(r[0]); return true;
        case G_ClearColor: glClearColor(AsF(r[0]), AsF(r[1]), AsF(r[2]), AsF(r[3])); return true;
        case G_ClearColorx: glClearColor(Fx(r[0]), Fx(r[1]), Fx(r[2]), Fx(r[3])); return true;
        case G_Color4x: glColor4f(Fx(r[0]), Fx(r[1]), Fx(r[2]), Fx(r[3])); return true;
        case G_CullFace: glCullFace(r[0]); return true;
        case G_DepthFunc: glDepthFunc(r[0]); return true;
        case G_DepthMask: glDepthMask(static_cast<GLboolean>(r[0] != 0)); return true;
        case G_DepthRangef: glDepthRange(AsF(r[0]), AsF(r[1])); return true;
        case G_DepthRangex: glDepthRange(Fx(r[0]), Fx(r[1])); return true;
        case G_Hint: glHint(r[0], r[1]); return true;
        case G_ShadeModel: glShadeModel(r[0]); return true;
        case G_PixelStorei: if (r[0] == GL_UNPACK_ALIGNMENT) S.unpack_align = r[1]; glPixelStorei(r[0], r[1]); return true;
        case G_Viewport:
            S.viewport[0] = r[0]; S.viewport[1] = r[1]; S.viewport[2] = r[2]; S.viewport[3] = r[3];
            ApplyViewport(); return true;
        case G_Scissor:
            S.scissor[0] = r[0]; S.scissor[1] = r[1]; S.scissor[2] = r[2]; S.scissor[3] = r[3];
            ApplyViewport(); return true;

        // fog / lighting / material
        case G_Fogx: if (r[0] == GL_FOG_MODE) glFogi(r[0], r[1]); else glFogf(r[0], Fx(r[1])); return true;
        case G_Fogxv: {
            if (r[0] == GL_FOG_MODE) { uint32_t m = 0; uc->MemRead(r[1], &m, 4); glFogi(r[0], m); return true; }
            float f[4]; ReadFixedVec(uc, r[1], r[0] == GL_FOG_COLOR ? 4 : 1, f); glFogfv(r[0], f); return true;
        }
        case G_LightModelx: if (r[0] == GL_LIGHT_MODEL_TWO_SIDE) glLightModeli(r[0], r[1]); else glLightModelf(r[0], Fx(r[1])); return true;
        case G_LightModelxv: {
            if (r[0] == GL_LIGHT_MODEL_TWO_SIDE) { uint32_t m = 0; uc->MemRead(r[1], &m, 4); glLightModeli(r[0], m); return true; }
            float f[4]; ReadFixedVec(uc, r[1], 4, f); glLightModelfv(r[0], f); return true;
        }
        case G_Lightxv: {
            int n = (r[1] == GL_SPOT_DIRECTION) ? 3 : (r[1] == GL_AMBIENT || r[1] == GL_DIFFUSE || r[1] == GL_SPECULAR || r[1] == GL_POSITION) ? 4 : 1;
            float f[4]; ReadFixedVec(uc, r[2], n, f); glLightfv(r[0], r[1], f); return true;
        }
        case G_Materialxv: {
            int n = (r[1] == GL_SHININESS) ? 1 : 4;
            float f[4]; ReadFixedVec(uc, r[2], n, f); glMaterialfv(r[0], r[1], f); return true;
        }

        // matrices
        case G_MatrixMode:
            S.matrix_mode = r[0];
            if (r[0] != GL_MATRIX_PALETTE_OES) { glMatrixMode(r[0]); S.host_matrix_mode = r[0]; }
            return true;
        case G_LoadIdentity:
            if (S.matrix_mode == GL_MATRIX_PALETTE_OES) { float* m = S.palette[S.cur_palette]; for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
            else glLoadIdentity();
            return true;
        case G_LoadMatrixx: {
            float m[16]; ReadFixedVec(uc, r[0], 16, m);
            if (S.matrix_mode == GL_MATRIX_PALETTE_OES) memcpy(S.palette[S.cur_palette], m, sizeof(m));
            else glLoadMatrixf(m);
            return true;
        }
        case G_PushMatrix: if (S.matrix_mode != GL_MATRIX_PALETTE_OES) glPushMatrix(); return true; // no palette stack in ES
        case G_PopMatrix:  if (S.matrix_mode != GL_MATRIX_PALETTE_OES) glPopMatrix(); return true;
        case G_Rotatex:
            if (S.matrix_mode == GL_MATRIX_PALETTE_OES) { float m[16]; RotationMatrix(Fx(r[0]), Fx(r[1]), Fx(r[2]), Fx(r[3]), m); MulMatrix(S.palette[S.cur_palette], m); }
            else glRotatef(Fx(r[0]), Fx(r[1]), Fx(r[2]), Fx(r[3]));
            return true;
        case G_Scalex:
            if (S.matrix_mode == GL_MATRIX_PALETTE_OES) { float m[16] = {Fx(r[0]),0,0,0, 0,Fx(r[1]),0,0, 0,0,Fx(r[2]),0, 0,0,0,1}; MulMatrix(S.palette[S.cur_palette], m); }
            else glScalef(Fx(r[0]), Fx(r[1]), Fx(r[2]));
            return true;
        case G_Translatex:
            if (S.matrix_mode == GL_MATRIX_PALETTE_OES) { float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, Fx(r[0]),Fx(r[1]),Fx(r[2]),1}; MulMatrix(S.palette[S.cur_palette], m); }
            else glTranslatef(Fx(r[0]), Fx(r[1]), Fx(r[2]));
            return true;
        case G_Frustumx: glFrustum(Fx(r[0]), Fx(r[1]), Fx(r[2]), Fx(r[3]), Fx(A(4)), Fx(A(5))); return true;
        case G_Orthox: glOrtho(Fx(r[0]), Fx(r[1]), Fx(r[2]), Fx(r[3]), Fx(A(4)), Fx(A(5))); return true;

        // textures
        case G_GenTextures: {
            std::vector<GLuint> ids(r[0]);
            glGenTextures(r[0], ids.data());
            for (uint32_t i = 0; i < r[0]; ++i) W32(r[1] + 4 * i, ids[i]);
            return true;
        }
        case G_DeleteTextures: {
            std::vector<GLuint> ids(r[0]);
            if (r[0]) uc->MemRead(r[1], ids.data(), 4 * r[0]);
            glDeleteTextures(r[0], ids.data());
            for (GLuint id : ids) { tex_filters_.erase(id); AssetViewer::OnDeleteTexture(id); CustomModel::ForgetTexture(id); }
            return true;
        }
        case G_BindTexture: glBindTexture(r[0], r[1]); return true;
        case G_TexParameterx:
            if (r[0] == GL_TEXTURE_2D && (r[1] == GL_TEXTURE_MIN_FILTER || r[1] == GL_TEXTURE_MAG_FILTER)) {
                GLint bound = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
                auto& f = tex_filters_.try_emplace(bound, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR).first->second;   // GL defaults
                (r[1] == GL_TEXTURE_MIN_FILTER ? f.first : f.second) = static_cast<int>(r[2]);
                glTexParameteri(r[0], r[1], EffectiveFilter(static_cast<int>(r[2])));
            } else {
                glTexParameteri(r[0], r[1], r[2]);
            }
            return true;
        case G_TexEnvx:
            if (r[1] == GL_RGB_SCALE || r[1] == GL_ALPHA_SCALE) glTexEnvf(r[0], r[1], Fx(r[2]));
            else glTexEnvi(r[0], r[1], r[2]);
            return true;
        case G_TexImage2D: { // target, level, ifmt, w, h, border, format, type, pixels
            uint32_t h = A(4), fmt = A(6), type = A(7), pix = A(8);
            std::vector<uint8_t> data;
            if (pix) data = ReadMem(uc, pix, ImageSize(r[3], h, fmt, type));
            if (r[0] == GL_TEXTURE_2D && ReplaceTexture(uc, static_cast<int32_t>(r[1]))) return true;
#ifdef MGS_VITA
            // vitaGL samples mip levels as noise, whether the game's or glGenerateMipmap's: level 0 alone, sampled
            // with the mipmapped filters' base filter (EffectiveFilter)
            if (static_cast<int32_t>(r[1]) > 0) return true;
            glTexImage2D(r[0], r[1], fmt, r[3], h, 0, fmt, type, pix ? data.data() : nullptr);
            ApplyBaseMinFilter();
#else
            glTexImage2D(r[0], r[1], fmt, r[3], h, 0, fmt, type, pix ? data.data() : nullptr);
#endif
            if (r[0] == GL_TEXTURE_2D && r[1] == 0) NoteTexture(uc, r[3], h, FormatName(fmt, type));
            return true;
        }
        case G_TexSubImage2D: { // target, level, x, y, w, h, format, type, pixels
            uint32_t w = A(4), h = A(5), fmt = A(6), type = A(7), pix = A(8);
            std::vector<uint8_t> data = ReadMem(uc, pix, ImageSize(w, h, fmt, type));
            glTexSubImage2D(r[0], r[1], r[2], r[3], w, h, fmt, type, data.data());
            return true;
        }
        case G_CompressedTexImage2D: { // target, level, ifmt, w, h, border, size, data
            uint32_t fmt = r[2];
            if (fmt >= GL_PALETTE4_RGB8_OES && fmt <= GL_PALETTE8_RGB5_A1_OES) {
                DecodePaletted(uc, r[0], static_cast<int32_t>(r[1]), fmt, r[3], A(4), A(6), A(7));
#ifdef MGS_VITA
                ApplyBaseMinFilter();
#endif
                static const char* kNames[] = { "PALETTE4_RGB8", "PALETTE4_RGBA8", "PALETTE4_R5G6B5", "PALETTE4_RGBA4",
                    "PALETTE4_RGB5_A1", "PALETTE8_RGB8", "PALETTE8_RGBA8", "PALETTE8_R5G6B5", "PALETTE8_RGBA4", "PALETTE8_RGB5_A1" };
                if (static_cast<int32_t>(r[1]) <= 0) NoteTexture(uc, r[3], A(4), kNames[fmt - GL_PALETTE4_RGB8_OES]);
            } else {
                char b[64]; snprintf(b, sizeof b, "unsupported compressed format 0x%x", fmt);
                WarnOnce(b);
            }
            return true;
        }
        default: break;
    }
    return true;
}

// ---- texture filtering toggle ---------------------------------------------------------------------------

// The filter actually set for a game-requested one: unchanged with filtering on; otherwise the
// nearest-neighbour equivalent (mipmapped modes keep their mip level selection but stop blending).
int GLES::EffectiveFilter(int requested) const {
#ifdef MGS_VITA
    switch (requested) {   // no mip levels on vitaGL (G_TexImage2D): level 0 alone
        case GL_NEAREST_MIPMAP_NEAREST: case GL_NEAREST_MIPMAP_LINEAR: requested = GL_NEAREST; break;
        case GL_LINEAR_MIPMAP_NEAREST: case GL_LINEAR_MIPMAP_LINEAR: requested = GL_LINEAR; break;
    }
#endif
    if (filter_applied_) return requested;
    switch (requested) {
        case GL_LINEAR: return GL_NEAREST;
        case GL_LINEAR_MIPMAP_LINEAR:
        case GL_LINEAR_MIPMAP_NEAREST:
        case GL_NEAREST_MIPMAP_LINEAR: return GL_NEAREST_MIPMAP_NEAREST;
        default: return requested;
    }
}

#ifdef MGS_VITA
// The bound texture's minification filter as EffectiveFilter has it, also for one the game never set a filter on
// (GL's default is mipmapped).
void GLES::ApplyBaseMinFilter() {
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    const auto it = tex_filters_.find(static_cast<uint32_t>(bound));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, EffectiveFilter(it != tex_filters_.end() ? it->second.first : GL_NEAREST_MIPMAP_LINEAR));
}
#endif

// Re-applies the filters of every texture the game configured, then restores the bindings.
void GLES::ApplyFilterMode() {
    filter_applied_ = filter_wanted_;
    std::cout << "[GLES] Texture filtering " << (filter_applied_ ? "on" : "off (nearest)") << std::endl;
    GLint active = GL_TEXTURE0, bound[2] = {};
    glGetIntegerv(0x84E0 /*GL_ACTIVE_TEXTURE*/, &active);
    for (int u = 0; u < 2; ++u) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound[u]);
    }
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    for (const auto& [id, f] : tex_filters_) {
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, EffectiveFilter(f.first));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, EffectiveFilter(f.second));
    }
    for (int u = 1; u >= 0; --u) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, bound[u]);
    }
    if (p_glActiveTexture) p_glActiveTexture(active);
}
