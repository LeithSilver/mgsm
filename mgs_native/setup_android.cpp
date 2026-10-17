// Android first-run setup (the counterpart of setup_window_win32.cpp): asks for the player's
// Metal.Gear.Solid.n-gage, either picked with the system file picker or copied into the app's folder, and
// installs it with Installer::Install. Drawn with SDL's renderer and its built-in debug font.
#ifdef __ANDROID__
#include "installer.h"
#include "platform.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
std::string g_picked;          // file picker result (content:// URI), set from SDL's dialog callback
bool g_picker_open = false;

void SDLCALL OnPicked(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_picker_open = false;
    if (files && files[0]) g_picked = files[0];
}

// A *.n-gage file in the app's folder (or its ROM/ subfolder), if the player copied one there.
fs::path FindPackage(const fs::path& dir) {
    std::error_code ec;
    for (const fs::path& d : {dir, dir / "ROM"})
        for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string ext = it->path().extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == ".n-gage" && it->is_regular_file(ec)) return it->path();
        }
    return {};
}

// The picked document (a content:// URI SDL can open) copied to a plain file the installer can read.
bool CopyUri(const std::string& uri, const fs::path& to, std::string* error) {
    SDL_IOStream* in = SDL_IOFromFile(uri.c_str(), "rb");
    if (!in) { *error = std::string("Could not open the chosen file: ") + SDL_GetError(); return false; }
    SDL_IOStream* out = SDL_IOFromFile(Platform::U8String(to).c_str(), "wb");
    if (!out) { SDL_CloseIO(in); *error = "Could not write to the app's folder."; return false; }
    std::vector<char> buf(1 << 16);
    size_t n;
    bool ok = true;
    while ((n = SDL_ReadIO(in, buf.data(), buf.size())) > 0) ok = ok && SDL_WriteIO(out, buf.data(), n) == n;
    SDL_CloseIO(in);
    ok = SDL_CloseIO(out) && ok;
    if (!ok) *error = "Could not copy the chosen file.";
    return ok;
}

// Lines no wider than `cols` characters (breaking long paths anywhere).
std::vector<std::string> Wrap(const std::string& text, size_t cols) {
    std::vector<std::string> out;
    size_t s = 0;
    while (s < text.size()) {
        size_t nl = text.find('\n', s);
        std::string para = text.substr(s, nl == std::string::npos ? std::string::npos : nl - s);
        s = nl == std::string::npos ? text.size() : nl + 1;
        if (para.empty()) { out.emplace_back(); continue; }
        while (!para.empty()) {
            size_t cut = para.size() <= cols ? para.size() : para.rfind(' ', cols);
            if (cut == std::string::npos || cut == 0) cut = std::min(cols, para.size());
            out.push_back(para.substr(0, cut));
            para.erase(0, cut);
            if (!para.empty() && para[0] == ' ') para.erase(0, 1);
        }
    }
    return out;
}

} // namespace

bool Installer::RunSetupScreen(const std::string& exe_dir, SDL_Window* window, SDL_Renderer* renderer) {
    const fs::path dir = fs::u8path(exe_dir);
    std::atomic<bool> busy{false}, done{false}, success{false};
    std::atomic<float> fraction{0.0f};
    std::string status, error;   // g_mutex
    std::thread worker;
    std::vector<SDL_Gamepad*> pads;
    auto start = [&](std::string source, bool is_uri) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true; done = false; fraction = 0.0f;
        { std::lock_guard<std::mutex> l(g_mutex); status = "Starting..."; error.clear(); }
        worker = std::thread([&, source, is_uri] {
            std::string err;
            fs::path pkg = fs::u8path(source);
            const fs::path copy = dir / "setup.n-gage.tmp";
            bool ok = true;
            if (is_uri) {
                { std::lock_guard<std::mutex> l(g_mutex); status = "Copying the package..."; }
                ok = CopyUri(source, copy, &err);
                pkg = copy;
            }
            ok = ok && Installer::Install(pkg, exe_dir, [&](float f, const std::string& s) {
                fraction = f;
                std::lock_guard<std::mutex> l(g_mutex);
                status = s;
            }, &err);
            std::error_code ec;
            if (is_uri) fs::remove(copy, ec);
            { std::lock_guard<std::mutex> l(g_mutex); error = ok ? "" : err; }
            success = ok;
            done = true;
            busy = false;
        });
    };
    auto pick = [&] {
        if (busy) return;
        { std::lock_guard<std::mutex> l(g_mutex); if (g_picker_open) return; g_picker_open = true; }
        SDL_ShowOpenFileDialog(OnPicked, nullptr, window, nullptr, 0, nullptr, false);
    };

    bool running = true;
    Uint64 last_scan = 0;
    fs::path last_found;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_GAMEPAD_ADDED: if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(p); break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) pick(); break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_SPACE) pick();
                    break;
                case SDL_EVENT_FINGER_UP: pick(); break;
                default: break;
            }
        }
        if (done && success) break;
        {
            std::string uri;
            { std::lock_guard<std::mutex> l(g_mutex); uri.swap(g_picked); }
            if (!uri.empty()) start(uri, true);
        }
        const Uint64 now = SDL_GetTicks();
        if (!busy && now - last_scan > 1000) {   // a package copied over USB / adb installs by itself
            last_scan = now;
            const fs::path found = FindPackage(dir);
            if (!found.empty() && found != last_found) { last_found = found; start(Platform::U8String(found), false); }
        }

        int w = 0, h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
        const float scale = std::max(1.0f, std::floor(w / (52.0f * 8.0f) * 2.0f) / 2.0f);
        const size_t cols = static_cast<size_t>(w / (8.0f * scale)) - 4;
        std::string text = "METAL GEAR SOLID MOBILE - SETUP\n\n"
            "This needs your copy of the N-Gage 2.0 game, the file Metal.Gear.Solid.n-gage.\n\n";
        {
            std::lock_guard<std::mutex> l(g_mutex);
            if (busy) text += status + "\n";
            else {
                text += "Press A (or tap the screen) to choose the file.\n\n"
                        "Or copy it into this folder (USB file transfer or adb push) and it installs by itself:\n" +
                        exe_dir + "\n";
                if (!error.empty()) text += "\nSetup failed: " + error + "\n";
            }
        }
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (busy) {   // progress bar along the bottom
            const SDL_FRect back = { w * 0.1f, h * 0.8f, w * 0.8f, h * 0.03f };
            SDL_FRect bar = back;
            bar.w *= std::clamp(fraction.load(), 0.0f, 1.0f);
            SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255); SDL_RenderFillRect(renderer, &back);
            SDL_SetRenderDrawColor(renderer, 120, 200, 120, 255); SDL_RenderFillRect(renderer, &bar);
        }
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        float y = 16.0f;
        for (const std::string& line : Wrap(text, cols)) { SDL_RenderDebugText(renderer, 16.0f, y, line.c_str()); y += 12.0f; }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    if (worker.joinable()) worker.join();
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    for (SDL_Gamepad* p : pads) SDL_CloseGamepad(p);
    return done && success;
}
#endif
