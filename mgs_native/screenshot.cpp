#include "screenshot.h"
#include "platform.h"
#include "pngio.h"

#include <cstdio>
#include <ctime>
#include <thread>

namespace fs = std::filesystem;

namespace {
fs::path NewPath() {
    std::error_code ec;
    const fs::path dir = Platform::ExeDir() / "screenshots";
    fs::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    for (int n = 0; n < 100; ++n) {
        char name[64];
        snprintf(name, sizeof name, n ? "mgs_%04d%02d%02d_%02d%02d%02d_%d.png" : "mgs_%04d%02d%02d_%02d%02d%02d.png",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, n);
        const fs::path p = dir / name;
        if (!fs::exists(p, ec)) return p;
    }
    return dir / "mgs_screenshot.png";
}
}

void Screenshot::SaveAsync(std::vector<uint32_t> bgra, int w, int h, void (*done)(const std::string&, const std::string&)) {
    for (uint32_t& p : bgra) p |= 0xFF000000u;   // GL alpha is whatever the game left there
    std::thread([px = std::move(bgra), w, h, done] {
        const fs::path path = NewPath();
        const std::string err = PngIO::Write(path, px.data(), w, h);
        if (done) done(err.empty() ? Platform::U8String(path) : "", err);
    }).detach();
}
