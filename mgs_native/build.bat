@echo off
setlocal
rem Windows build: MSVC x64 + Ninja through CMake (mgs_native\CMakeLists.txt) -> mgs_mobile.exe, unicorn.dll and
rem SDL3.dll in the repo root. Run from the repo root. Intermediate files go to build\.
echo Setting up MSVC environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul

rem Unicorn headers / import lib / DLL come from the Python "unicorn" package (pip install unicorn==2.1.4)
python -c "import unicorn" 2>nul
if errorlevel 1 (
    echo Build Failed: the Python unicorn package was not found ^(pip install unicorn==2.1.4^)
    exit /b 1
)

echo Building the icon resource...
python tools\make_icon.py || echo Warning: could not regenerate mgs_native\mgs_mobile.ico, using the existing one

rem Optional Dynarmic CPU backend (README.md section 6.27): set MGS_DYNARMIC_DIR to a dynarmic checkout and
rem MGS_BOOST_CMAKE_DIR to a folder with BoostConfig.cmake, then delete build\ so it reconfigures.
set DYNARMIC_ARGS=
if defined MGS_DYNARMIC_DIR set DYNARMIC_ARGS=-DMGS_DYNARMIC_DIR=%MGS_DYNARMIC_DIR% -DMGS_BOOST_CMAKE_DIR=%MGS_BOOST_CMAKE_DIR%

if not exist build\build.ninja (
    echo Configuring...
    cmake -S mgs_native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %DYNARMIC_ARGS%
    if errorlevel 1 (
        echo Build Failed: CMake configuration error
        exit /b 1
    )
)

echo Compiling mgs_mobile.exe...
cmake --build build
if %ERRORLEVEL% equ 0 (
    echo.
    echo ========================================================
    echo  Build Successful: mgs_mobile.exe generated!
    echo ========================================================
) else (
    echo Build Failed with error %ERRORLEVEL%
    exit /b 1
)
