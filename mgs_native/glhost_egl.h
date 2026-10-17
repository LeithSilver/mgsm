#pragma once

#include "gles.h"

// Android: an OpenGL ES 1.1 context on an EGL pbuffer for gles.cpp (glhost_egl.cpp). make_current() creates it
// and binds it to the calling thread, once.
GLES::Host EglPbufferHost();
