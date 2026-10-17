#!/bin/bash
# Linux (and other non-Windows) build: CMake + Ninja (mgs_native/CMakeLists.txt) -> ./mgs_mobile in the repo root.
# Run from the repo root. Intermediate files go to $MGS_BUILD_DIR (default build-linux/).
#
# Needs: a C++17 compiler, CMake >= 3.16, Ninja (or set MGS_GENERATOR="Unix Makefiles"), desktop OpenGL headers,
#   Unicorn 2.1.4 (pip install --user unicorn==2.1.4, or -DUNICORN_ROOT=...), and SDL 3 (a distro package, or built
#   from source; point SDL3_DIR at its lib/cmake/SDL3 folder). pip install --user cmake ninja works without root.
set -e
export PATH="$HOME/.local/bin:$PATH"
B="${MGS_BUILD_DIR:-build-linux}"
if [ ! -f "$B/CMakeCache.txt" ]; then
    echo "Configuring..."
    cmake -S mgs_native -B "$B" -G "${MGS_GENERATOR:-Ninja}" -DCMAKE_BUILD_TYPE=Release \
          ${SDL3_DIR:+-DSDL3_DIR="$SDL3_DIR"} "$@"
fi
echo "Compiling mgs_mobile..."
if cmake --build "$B"; then
    echo
    echo "========================================================"
    echo " Build Successful: mgs_mobile generated!"
    echo "========================================================"
else
    echo "Build Failed"
    exit 1
fi
