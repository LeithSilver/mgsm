# Android native build (driven by Gradle, android/app/build.gradle): SDL 3 as libSDL3.so, then the game as
# libmain.so from mgs_native/, linked against Unicorn built for this ABI by android/build.py.
cmake_minimum_required(VERSION 3.16)
project(mgs_android C CXX)

if(NOT MGS_ANDROID_DEPS OR NOT MGS_SDL_DIR)
    message(FATAL_ERROR "MGS_ANDROID_DEPS / MGS_SDL_DIR not set: build with android/build.py")
endif()

# Debug APKs are still optimised: the game runs on a CPU emulator and is too slow at -O0
string(APPEND CMAKE_C_FLAGS_DEBUG " -O2")
string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O2")

# 16 KB page devices (Android 15+, e.g. Pixel 10): every shared library needs 16 KB aligned segments
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")

# Source paths are compiled into the libraries (assertions, __FILE__): mapped to relative ones, so the APK carries no
# folder names of the build machine (one option each: the repo path has spaces)
get_filename_component(MGS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${MGS_REPO_ROOT}/=" "-ffile-prefix-map=${MGS_ANDROID_DEPS}/=")

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${MGS_SDL_DIR}" SDL3)

set(UNICORN_ROOT "${MGS_ANDROID_DEPS}/unicorn-2.1.4" CACHE PATH "" FORCE)
set(UNICORN_LIB "${MGS_ANDROID_DEPS}/build-unicorn-${ANDROID_ABI}/libunicorn.a" CACHE FILEPATH "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../mgs_native" mgs)
