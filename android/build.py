#!/usr/bin/env python3
"""Android build of the MGS Mobile port: python android/build.py [--release] [--install] [--run] [--sdk DIR]

1. Native dependencies in MGS_ANDROID_DEPS (default ~/mgs-android-deps; no spaces allowed: Unicorn's configure
   script can't handle them): the SDL 3 and Unicorn source releases (downloaded once, SHA-256 checked), and
   Unicorn built as a static library for each ABI (MGS_ANDROID_ABIS, default arm64-v8a) with the NDK.
2. Launcher icons from icon.png (app/build/generated/mgs-icons).
3. Gradle (android/gradlew): SDL and mgs_native via app/jni/CMakeLists.txt -> app/build/outputs/apk/.../*.apk
4. --install: adb install -r; --run: also start it and follow the log (adb logcat -s MGS).

Needs: the Android SDK (ANDROID_HOME, --sdk, or android/local.properties) with NDK 24.0.8215888, CMake 3.22.1
and platform 33; a JDK 17 (JAVA_HOME, or Android Studio's); Python with Pillow (icons). On Windows also a POSIX
sh (Git for Windows), for Unicorn's configure step.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
NDK_VERSION = "24.0.8215888"
CMAKE_VERSION = "3.22.1"
MIN_API = 24
DEPS = {
    "SDL3-3.4.18": ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz",
                    "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"),
    "unicorn-2.1.4": ("https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz",
                      "ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4"),
}
ICON_SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
WIN = os.name == "nt"


def fail(msg):
    sys.exit(f"[android] {msg}")


def run(cmd, **kw):
    print("[android] $", " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode != 0:
        fail(f"failed ({r.returncode}): {cmd[0]}")
    return r


def find_sdk(arg):
    cands = [arg, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT")]
    lp = HERE / "local.properties"
    if lp.exists():
        for line in lp.read_text().splitlines():
            if line.startswith("sdk.dir="):
                cands.append(line[8:].replace("\\:", ":").replace("\\\\", "\\"))
    cands += [os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"), str(Path.home() / "Android" / "Sdk")]
    for c in cands:
        if c and (Path(c) / "platform-tools").is_dir():
            return Path(c)
    fail("Android SDK not found: set ANDROID_HOME or pass --sdk")


def find_java():
    if os.environ.get("JAVA_HOME"):
        return os.environ["JAVA_HOME"]
    roots = [Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Android" / "Android Studio",
             Path("/opt/android-studio"), Path.home() / "android-studio"]
    for r in roots:
        for sub in ("jbr", "OpenJDK/jdk-17.0.2"):
            if (r / sub / "bin").is_dir():
                return str(r / sub)
        for jdk in sorted((r / "OpenJDK").glob("jdk-17*")) if (r / "OpenJDK").is_dir() else []:
            return str(jdk)
    return None   # whatever java is on PATH


def fetch_deps(deps):
    deps.mkdir(parents=True, exist_ok=True)
    for name, (url, sha) in DEPS.items():
        if (deps / name).is_dir():
            continue
        tgz = deps / f"{name}.tar.gz"
        if not tgz.exists():
            print(f"[android] Downloading {url}", flush=True)
            urllib.request.urlretrieve(url, tgz)
        got = hashlib.sha256(tgz.read_bytes()).hexdigest()
        if got != sha:
            tgz.unlink()
            fail(f"{tgz.name}: SHA-256 mismatch ({got}), deleted; run again to re-download")
        with tarfile.open(tgz) as t:
            t.extractall(deps, filter="data")
        if not (deps / name).is_dir():
            fail(f"{tgz.name} did not unpack to {name}/")


def build_unicorn(deps, sdk, abi):
    out = deps / f"build-unicorn-{abi}"
    if (out / "libunicorn.a").exists():
        return
    ndk = sdk / "ndk" / NDK_VERSION
    cmake_bin = sdk / "cmake" / CMAKE_VERSION / "bin"
    if not ndk.is_dir() or not cmake_bin.is_dir():
        fail(f"needs NDK {NDK_VERSION} and CMake {CMAKE_VERSION} in the SDK (sdkmanager \"ndk;{NDK_VERSION}\" \"cmake;{CMAKE_VERSION}\")")
    env = dict(os.environ)
    # QEMU's configure (run by Unicorn's CMake) needs sh, strings and pkg-config; the NDK has llvm-strings,
    # and pkg-config only has to exist.
    shims = deps / "shims"
    shims.mkdir(exist_ok=True)
    path = [str(shims), str(cmake_bin)]
    if WIN:
        sh = shutil.which("sh") or next((str(p) for p in [Path(os.environ.get("ProgramFiles", "")) / "Git" / "usr" / "bin" / "sh.exe"] if p.exists()), None)
        if not sh:
            fail("needs a POSIX sh for Unicorn's configure step (install Git for Windows)")
        path.append(str(Path(sh).parent))
    if not shutil.which("strings"):
        strings = next(ndk.glob("toolchains/llvm/prebuilt/*/bin/llvm-strings*"))
        shutil.copy(strings, shims / ("strings.exe" if WIN else "strings"))
    if not shutil.which("pkg-config"):
        (shims / "pkg-config").write_text("#!/bin/sh\nexit 1\n", newline="\n")
        (shims / "pkg-config").chmod(0o755)
    env["PATH"] = os.pathsep.join(path + [env.get("PATH", "")])
    run([cmake_bin / "cmake", "-S", deps / "unicorn-2.1.4", "-B", out, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={(ndk / 'build/cmake/android.toolchain.cmake').as_posix()}",
         f"-DANDROID_ABI={abi}", f"-DANDROID_PLATFORM=android-{MIN_API}", "-DCMAKE_BUILD_TYPE=Release",
         "-DUNICORN_ARCH=arm", "-DUNICORN_BUILD_TESTS=OFF", "-DUNICORN_INSTALL=OFF", "-DBUILD_SHARED_LIBS=OFF",
         # (source paths compiled into QEMU's assertions: relative, so the APK names no folders of this machine)
         f"-DCMAKE_C_FLAGS=-ffile-prefix-map={deps.as_posix()}/="], env=env)
    if not (out / "config-host.h").exists():
        shutil.rmtree(out, ignore_errors=True)
        fail("Unicorn's configure step failed (no config-host.h); see the output above")
    run([cmake_bin / "cmake", "--build", out], env=env)


def make_icons():
    out = HERE / "app" / "build" / "generated" / "mgs-icons"
    src = REPO / "icon.png"
    marker = out / "mipmap-xxxhdpi" / "ic_launcher.png"
    if marker.exists() and marker.stat().st_mtime >= src.stat().st_mtime:
        return
    from PIL import Image
    img = Image.open(src).convert("RGBA")
    for dpi, size in ICON_SIZES.items():
        d = out / f"mipmap-{dpi}"
        d.mkdir(parents=True, exist_ok=True)
        img.resize((size, size), Image.LANCZOS).save(d / "ic_launcher.png")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--release", action="store_true", help="optimised build (still signed with the debug key)")
    ap.add_argument("--install", action="store_true", help="adb install the APK")
    ap.add_argument("--run", action="store_true", help="install, start, and show the log")
    ap.add_argument("--sdk", help="Android SDK folder")
    ap.add_argument("--device", help="adb device serial")
    a = ap.parse_args()

    deps = Path(os.environ.get("MGS_ANDROID_DEPS", Path.home() / "mgs-android-deps")).resolve()
    if " " in str(deps):
        fail(f"{deps}: the dependency folder can't contain spaces; set MGS_ANDROID_DEPS")
    sdk = find_sdk(a.sdk)
    lp = HERE / "local.properties"
    if not lp.exists():
        lp.write_text("sdk.dir=" + str(sdk).replace("\\", "\\\\").replace(":", "\\:") + "\n")
    fetch_deps(deps)
    abis = os.environ.get("MGS_ANDROID_ABIS", "arm64-v8a").split(",")
    for abi in abis:
        build_unicorn(deps, sdk, abi)
    make_icons()

    env = dict(os.environ, MGS_ANDROID_DEPS=str(deps), MGS_ANDROID_ABIS=",".join(abis), ANDROID_HOME=str(sdk))
    java = find_java()
    if java:
        env["JAVA_HOME"] = java
    variant = "Release" if a.release else "Debug"
    gradlew = HERE / ("gradlew.bat" if WIN else "gradlew")
    run([gradlew, f"assemble{variant}", "--console=plain"], cwd=HERE, env=env)
    apk = next((HERE / "app" / "build" / "outputs" / "apk" / variant.lower()).glob("*.apk"))
    print(f"[android] Built {apk}")

    if a.install or a.run:
        adb = [sdk / "platform-tools" / ("adb.exe" if WIN else "adb")] + (["-s", a.device] if a.device else [])
        run(adb + ["install", "-r", apk])
        if a.run:
            run(adb + ["logcat", "-c"])
            run(adb + ["shell", "am", "start", "-n", "org.mgsmobile.port/.MGSActivity"])
            subprocess.run([str(c) for c in adb + ["logcat", "-s", "MGS:V", "SDL:V", "AndroidRuntime:E", "DEBUG:V", "libc:V"]])


if __name__ == "__main__":
    main()
