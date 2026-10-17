@echo off
rem 3DS build (README.md section 6.28): runs make in the devkitPro MSYS2 shell. Output: mgs_mobile.3dsx here.
C:\devkitPro\msys2\usr\bin\bash.exe -lc "source /etc/profile.d/devkit-env.sh && make -j8 -C \"$(cygpath -u '%~dp0')\" %*"
