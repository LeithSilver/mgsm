#pragma once

// Host GL headers. Desktop: OpenGL, compatibility profile. MGS_GLES (Android): OpenGL ES 1.1, which has the same
// fixed-function pipeline; the few desktop-only calls gles.cpp makes are mapped to their ES equivalents below.
#ifdef MGS_GLES
#include <GLES/gl.h>
#include <GLES/glext.h>
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
inline void glDepthRange(double n, double f) { glDepthRangef(static_cast<GLfloat>(n), static_cast<GLfloat>(f)); }
inline void glFrustum(double l, double r, double b, double t, double n, double f) {
    glFrustumf(static_cast<GLfloat>(l), static_cast<GLfloat>(r), static_cast<GLfloat>(b), static_cast<GLfloat>(t),
               static_cast<GLfloat>(n), static_cast<GLfloat>(f));
}
inline void glOrtho(double l, double r, double b, double t, double n, double f) {
    glOrthof(static_cast<GLfloat>(l), static_cast<GLfloat>(r), static_cast<GLfloat>(b), static_cast<GLfloat>(t),
             static_cast<GLfloat>(n), static_cast<GLfloat>(f));
}
inline void glFogi(GLenum p, GLint v) { glFogf(p, static_cast<GLfloat>(v)); }
inline void glLightModeli(GLenum p, GLint v) { glLightModelf(p, static_cast<GLfloat>(v)); }
#else
// On Windows <GL/gl.h> needs <windows.h> first.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <GL/gl.h>
#endif

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058   // (ES: GL_RGBA8_OES)
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif

// Saves the GL state the game could notice and restores it when it goes out of scope, around our own drawing
// (the overlay, the paused frame, software frames). Desktop: glPushAttrib; ES has no attribute stack, so the
// state we touch is read back one by one. Matrices are not included (push/pop them). Client arrays are not
// saved: gles.cpp sets them up for every guest draw.
class GLStateSave {
public:
    GLStateSave();
    ~GLStateSave();
    GLStateSave(const GLStateSave&) = delete;
    GLStateSave& operator=(const GLStateSave&) = delete;
private:
#ifdef MGS_GLES
    static constexpr int kCaps = 13;
    GLboolean caps_[kCaps] = {}, tex2d_[2] = {}, color_mask_[4] = {}, depth_mask_ = GL_TRUE;
    GLint active_ = 0, client_ = 0, bound_[2] = {}, env_mode_[2] = {}, viewport_[4] = {}, scissor_[4] = {};
    GLint blend_src_ = 0, blend_dst_ = 0, shade_ = 0, unpack_ = 4, pack_ = 4, matrix_mode_ = 0;
    GLfloat color_[4] = {};
#endif
};

// Draws texture `tex` over the whole viewport (texture row 0 at the top when flip_v, else at the bottom) with
// state set up for a plain copy. Call inside a GLStateSave; leaves the matrices as they were.
void GLDrawFullscreenTexture(GLuint tex, bool flip_v);
