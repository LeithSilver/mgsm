// Android: the OpenGL ES 1.1 context gles.cpp renders with, on a small pbuffer (all drawing goes into FBOs and is
// read back, like the desktop's hidden-window context). Created with EGL directly: SDL's window holds its own
// context for the 2D renderer that shows the frames, and Android allows only that one window. It lives until the
// process ends.
#ifdef __ANDROID__
#include "glhost_egl.h"
#include <EGL/egl.h>
#include <dlfcn.h>
#include <iostream>

namespace {
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLContext g_ctx = EGL_NO_CONTEXT;

bool CreateAndBind() {   // the calling thread (the emu thread) keeps the context current from here on
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, nullptr, nullptr)) {
        std::cerr << "[EGL] No display (0x" << std::hex << eglGetError() << std::dec << ")" << std::endl;
        return false;
    }
    const EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, attrs, &cfg, 1, &n) || n < 1) {
        std::cerr << "[EGL] No OpenGL ES 1 pbuffer config (0x" << std::hex << eglGetError() << std::dec << ")" << std::endl;
        return false;
    }
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    const EGLint surf_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, surf_attrs);
    if (g_ctx == EGL_NO_CONTEXT || g_surf == EGL_NO_SURFACE || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        std::cerr << "[EGL] Could not create the OpenGL ES 1 context (0x" << std::hex << eglGetError() << std::dec << ")" << std::endl;
        return false;
    }
    return true;
}
} // namespace

GLES::Host EglPbufferHost() {
    return {
        [] { return CreateAndBind(); },
        [](const char* name) -> void* {
            // Core ES 1.1 functions from the linked libGLESv1_CM, extensions through EGL
            void* p = dlsym(RTLD_DEFAULT, name);
            return p ? p : reinterpret_cast<void*>(eglGetProcAddress(name));
        } };
}

#endif
