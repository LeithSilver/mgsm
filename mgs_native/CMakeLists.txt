# MGS Mobile native port. Windows: mgs_native\build.bat (MSVC + Ninja, wraps this). Linux: mgs_native/build.sh.
# Android: android/build.py (Gradle; android/app/jni/CMakeLists.txt adds SDL and this directory). Elsewhere:
#   cmake -S mgs_native -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# The executable goes to MGS_OUTPUT_DIR (default: the repo root, next to the game files), with its DLLs on Windows.
# Android builds libmain.so instead, which SDL's Java activity loads.
#
# Dependencies
#   Unicorn 2.1.4: from the Python "unicorn" package by default (pip install unicorn==2.1.4: headers + library),
#                  or -DUNICORN_ROOT=<dir with include/> -DUNICORN_LIB=<library> (Android: built by android/build.py).
#   SDL3:          Windows: the vendored third_party/SDL3 (VC dev package); Android: the SDL3::SDL3 target of the
#                  enclosing project; elsewhere find_package(SDL3).
#   OpenGL:        desktop GL, compatibility profile; Android: OpenGL ES 1.1 + EGL (MGS_GLES).
cmake_minimum_required(VERSION 3.16)
project(mgs_mobile C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(MGS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MGS_OUTPUT_DIR "${MGS_REPO_ROOT}" CACHE PATH "Where the executable is written")
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ---- Unicorn ---------------------------------------------------------------------------------------------
set(UNICORN_ROOT "" CACHE PATH "Unicorn install (include/, lib/); empty = the Python unicorn package")
if(NOT UNICORN_ROOT)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import os, unicorn; print(os.path.dirname(unicorn.__file__))"
                    OUTPUT_VARIABLE UNICORN_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _uc_res)
    if(NOT _uc_res EQUAL 0 OR NOT UNICORN_ROOT)
        message(FATAL_ERROR "Python unicorn package not found (pip install unicorn==2.1.4) and UNICORN_ROOT not set")
    endif()
endif()
if(WIN32)
    set(UNICORN_LIB "${UNICORN_ROOT}/lib/unicorn.lib")
    set(UNICORN_DLL "${UNICORN_ROOT}/lib/unicorn.dll")
else()
    find_library(UNICORN_LIB NAMES unicorn libunicorn.so.2 PATHS "${UNICORN_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT UNICORN_LIB)
        find_library(UNICORN_LIB NAMES unicorn)
    endif()
endif()
if(NOT EXISTS "${UNICORN_ROOT}/include/unicorn/unicorn.h" OR NOT UNICORN_LIB)
    message(FATAL_ERROR "Unicorn headers/library not found under ${UNICORN_ROOT}")
endif()
add_library(unicorn UNKNOWN IMPORTED)
set_target_properties(unicorn PROPERTIES IMPORTED_LOCATION "${UNICORN_LIB}"
                      INTERFACE_INCLUDE_DIRECTORIES "${UNICORN_ROOT}/include")
message(STATUS "Unicorn: ${UNICORN_LIB}")

# ---- SDL3 ------------------------------------------------------------------------------------------------
if(TARGET SDL3::SDL3)
    # (provided by the enclosing project: Android)
elseif(WIN32)
    add_library(SDL3::SDL3 SHARED IMPORTED)
    set_target_properties(SDL3::SDL3 PROPERTIES
        IMPORTED_IMPLIB "${TP}/SDL3/lib/x64/SDL3.lib"
        IMPORTED_LOCATION "${TP}/SDL3/lib/x64/SDL3.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${TP}/SDL3/include")
else()
    find_package(SDL3 REQUIRED CONFIG)
endif()

if(NOT ANDROID)
    find_package(OpenGL REQUIRED)
endif()

# ---- the game ----------------------------------------------------------------------------------------------
set(MGS_SOURCES
    loader.cpp vfs.cpp runtime.cpp main.cpp gles.cpp heap.cpp audio.cpp gamepad.cpp settings.cpp overlay.cpp
    luachunk.cpp story.cpp gametools.cpp photomode.cpp screenshot.cpp pngio.cpp textures.cpp custommodel.cpp installer.cpp
    autotest.cpp platform.cpp keys.cpp cpu_unicorn.cpp cpu_native.cpp cpu_dynarmic.cpp guest_interp.cpp touchcontrols.cpp movement.cpp assetviewer.cpp modelsandbox.cpp
    ${TP}/miniz/miniz.c ${TP}/lzma/LzmaDec.c
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp ${TP}/imgui/imgui_widgets.cpp)
# Dynarmic CPU backend (optional): MGS_DYNARMIC_DIR = a dynarmic checkout with its externals (azahar-emu/dynarmic),
# MGS_BOOST_CMAKE_DIR = a folder with a BoostConfig.cmake for Boost's headers (dynarmic uses ICL and variant).
set(MGS_DYNARMIC_DIR "" CACHE PATH "dynarmic source tree (empty: no Dynarmic backend)")
set(MGS_BOOST_CMAKE_DIR "" CACHE PATH "folder with BoostConfig.cmake for dynarmic")
if(MGS_DYNARMIC_DIR)
    set(Boost_DIR "${MGS_BOOST_CMAKE_DIR}" CACHE PATH "" FORCE)
    set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
    set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
    add_subdirectory("${MGS_DYNARMIC_DIR}" dynarmic EXCLUDE_FROM_ALL)
endif()

# Native CPU backend's context switch (32-bit ARM hosts only; cpu_native.cpp is a stub elsewhere)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7.*)$" OR ANDROID_ABI STREQUAL "armeabi-v7a")
    enable_language(ASM)
    list(APPEND MGS_SOURCES native_arm.S)
endif()
if(ANDROID)
    list(APPEND MGS_SOURCES glhost_egl.cpp setup_android.cpp imgui_impl_gles1.cpp)   # ES context, setup screen, overlay
else()
    list(APPEND MGS_SOURCES ${TP}/imgui/backends/imgui_impl_opengl2.cpp)
endif()
if(WIN32)
    list(APPEND MGS_SOURCES mgs_mobile.rc setup_window_win32.cpp)   # exe + window icon (resource 1); setup window
endif()

if(ANDROID)
    add_library(mgs_mobile SHARED ${MGS_SOURCES})
    set_target_properties(mgs_mobile PROPERTIES OUTPUT_NAME main)   # libmain.so, what SDLActivity loads
    target_compile_definitions(mgs_mobile PRIVATE MGS_GLES)
    target_link_libraries(mgs_mobile PRIVATE GLESv1_CM EGL android log)
    # 16 KB page devices (Android 15+) need 16 KB aligned segments
    target_link_options(mgs_mobile PRIVATE "-Wl,-z,max-page-size=16384")
else()
    add_executable(mgs_mobile ${MGS_SOURCES})
    target_link_libraries(mgs_mobile PRIVATE OpenGL::GL)
endif()
target_include_directories(mgs_mobile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${TP} ${TP}/imgui ${TP}/miniz ${TP}/lzma)
target_link_libraries(mgs_mobile PRIVATE unicorn SDL3::SDL3)
if(MGS_DYNARMIC_DIR)
    target_link_libraries(mgs_mobile PRIVATE dynarmic merry::mcl Boost::boost)   # (mcl, Boost: for the IR headers the hooks use)
    target_compile_definitions(mgs_mobile PRIVATE MGS_DYNARMIC)
    # (dynarmic's headers need C++20; the rest stays C++17)
    if(MSVC)
        set_source_files_properties(cpu_dynarmic.cpp PROPERTIES COMPILE_OPTIONS "/std:c++20")
    else()
        set_source_files_properties(cpu_dynarmic.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
    endif()
endif()
set_target_properties(mgs_mobile PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MGS_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MGS_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MGS_OUTPUT_DIR}")

if(MSVC)
    # Reproducible, no debug info: the exe holds no source paths or build time (tools/make_dist.py checks).
    target_compile_options(mgs_mobile PRIVATE /Brepro)
    target_link_options(mgs_mobile PRIVATE /Brepro)
    target_compile_definitions(mgs_mobile PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Win32 APIs still used: the first-run setup window (installer.cpp: GDI, file dialog, drag and drop)
    target_link_libraries(mgs_mobile PRIVATE user32 gdi32 shell32 comdlg32)
    add_custom_command(TARGET mgs_mobile POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNICORN_DLL}" "${TP}/SDL3/lib/x64/SDL3.dll" "${MGS_OUTPUT_DIR}")
elseif(ANDROID)
    target_compile_options(mgs_mobile PRIVATE -Wall -Wno-unused-parameter)
else()
    target_compile_options(mgs_mobile PRIVATE -Wall -Wno-unused-parameter)
    target_link_libraries(mgs_mobile PRIVATE pthread)
    # Find libunicorn / SDL3 next to the executable when they're shipped alongside it
    set_target_properties(mgs_mobile PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
