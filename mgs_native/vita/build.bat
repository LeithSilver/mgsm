@echo off
rem PS Vita build (README.md): runs build.sh with VitaSDK in WSL Ubuntu (~/vitasdk). Output: build\mgs_mobile.vpk, build\eboot.bin.
wsl -d Ubuntu --cd "%~dp0" --exec bash ./build.sh %*
