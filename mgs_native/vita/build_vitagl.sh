#!/bin/bash
# vitaGL from source at a tested commit, installed over the vdpm package (README.md §6.29). The nightly package
# (2026-10) crashed in glBlitFramebuffer after the F1 overlay's ImGui draws; this commit doesn't. Run once per
# VitaSDK install (build.sh warns while it's missing). The vdpm library is kept as libvitaGL.a.vdpm.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:/usr/local/bin:/usr/bin:/bin"
COMMIT=7635e3c
SRC="$HOME/vitagl-src"
LIB="$VITASDK/arm-vita-eabi/lib"
if [ ! -d "$SRC/.git" ]; then rm -rf "$SRC"; git clone https://github.com/Rinnegatamante/vitaGL.git "$SRC"; fi
cd "$SRC"
git fetch -q origin || true
git checkout -q --force "$COMMIT"
make clean > /dev/null
make -j8 NO_DEBUG=1 NO_SPLASHSCREEN=1 > build.log 2>&1 || { tail -20 build.log; exit 1; }
[ -f "$LIB/libvitaGL.a.vdpm" ] || cp "$LIB/libvitaGL.a" "$LIB/libvitaGL.a.vdpm"
cp libvitaGL.a "$LIB/libvitaGL.a"
cp source/vitaGL.h "$VITASDK/arm-vita-eabi/include/vitaGL.h"
echo "$COMMIT" > "$LIB/libvitaGL.mgs"
rm -f "$HERE/build/mgs_mobile"   # (relink: CMake doesn't see a changed library)
echo "vitaGL $COMMIT installed"
