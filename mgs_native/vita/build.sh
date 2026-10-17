#!/bin/bash
# PS Vita build with VitaSDK (~/vitasdk in WSL). Output: build/mgs_mobile.vpk, build/eboot.bin.
set -e
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:/usr/local/bin:/usr/bin:/bin:$HOME/.local/bin"
cd "$(dirname "$0")"
[ -f "$VITASDK/arm-vita-eabi/lib/libvitaGL.mgs" ] || echo "warning: vdpm's vitaGL crashes with the F1 menu; run build_vitagl.sh once"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j8 "$@"
echo "Build Successful"
