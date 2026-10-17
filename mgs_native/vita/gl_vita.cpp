// GL ES 1.1 calls gles.cpp makes that vitaGL doesn't have, built on ones it does (README.md §6.29).
#include <vitaGL.h>

extern "C" void glLightModelf(GLenum pname, GLfloat param) { glLightModelfv(pname, &param); }
