#include "platform.h"

#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#include <cstdio>
#include <thread>
#endif

std::filesystem::path Platform::ExeDir() {
#ifdef __ANDROID__
    // The app's folder in shared storage (Android/data/<package>/files): reachable over USB / adb, no permission
    // needed. Internal storage if that is unavailable.
    static const std::filesystem::path dir = [] {
        const char* p = SDL_GetAndroidExternalStoragePath();
        if (!p) p = SDL_GetAndroidInternalStoragePath();
        std::string s = p ? p : ".";
        if (s.back() != '/') s += '/';
        return U8Path(s);
    }();
    return dir;
#else
    const char* base = SDL_GetBasePath();   // UTF-8, ends with a separator; owned by SDL
    return base ? U8Path(base) : std::filesystem::current_path();
#endif
}

void Platform::StartLog() {
#ifdef __ANDROID__
    // stdout/stderr go nowhere in an app: send them to logcat (tag MGS) and mgs_mobile.log in ExeDir().
    static int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    FILE* file = fopen(U8String(ExeDir() / "mgs_mobile.log").c_str(), "w");
    std::thread([file] {
        char buf[2048];
        std::string line;
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof buf)) > 0) {
            if (file) { fwrite(buf, 1, static_cast<size_t>(n), file); fflush(file); }
            line.append(buf, static_cast<size_t>(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
                __android_log_write(ANDROID_LOG_INFO, "MGS", line.substr(0, nl).c_str());
        }
    }).detach();
    // Debug environment variables (README.md §7), which can't be set for an app: mgs_env.txt in ExeDir(),
    // one NAME=value per line.
    if (FILE* env = fopen(U8String(ExeDir() / "mgs_env.txt").c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof line, env)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const size_t eq = l.find('=');
            if (eq != std::string::npos && eq > 0 && l[0] != '#') setenv(l.substr(0, eq).c_str(), l.substr(eq + 1).c_str(), 1);
        }
        fclose(env);
    }
#endif
}
