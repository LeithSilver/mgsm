#include "autotest.h"
#include "pngio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {
std::thread g_thread;
std::atomic<bool> g_stop{false};

// false once Stop() was called
bool Sleep(double s) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(s * 1e6));
    while (!g_stop && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return !g_stop;
}

std::vector<std::string> Split(const char* env) {
    std::vector<std::string> out;
    std::stringstream ss(env ? env : "");
    for (std::string item; std::getline(ss, item, ',');) if (!item.empty()) out.push_back(item);
    return out;
}
}

bool AutoTest::Start(Hooks hooks) {
    const char* spec = getenv("MGS_AUTOTEST");
    if (!spec) return false;
    int steps = 6, key_every = 1;
    double interval = 2.0;
    sscanf(spec, "%d,%lf,%d", &steps, &interval, &key_every);
    if (key_every < 1) key_every = 1;
    const double default_hold = getenv("MGS_KEY_HOLD") ? atof(getenv("MGS_KEY_HOLD")) : 0.1;
    std::map<int, std::pair<int, double>> keys;   // step -> (vk, hold)
    for (const std::string& kv : Split(getenv("MGS_KEY_SCRIPT"))) {
        int step = 0; unsigned vk = 0; double hold = default_hold;
        if (sscanf(kv.c_str(), "%d:%x/%lf", &step, &vk, &hold) >= 2) keys[step] = { static_cast<int>(vk), hold };
    }
    std::map<int, std::pair<int, int>> resizes;   // step -> (w, h)
    for (const std::string& kv : Split(getenv("MGS_RESIZE_SCRIPT"))) {
        int step = 0, w = 0, h = 0;
        if (sscanf(kv.c_str(), "%d:%dx%d", &step, &w, &h) == 3) resizes[step] = { w, h };
    }
    std::cout << "[AutoTest] " << steps << " steps, " << interval << " s apart, Enter every " << key_every << std::endl;

    g_thread = std::thread([=] {
        if (!Sleep(2.0)) return;   // window up, game booting (as the old external harness waited)
        for (int step = 0; step < steps; ++step) {
            if (auto r = resizes.find(step); r != resizes.end()) hooks.resize(r->second.first, r->second.second);
            if (!Sleep(interval)) return;
            std::vector<uint32_t> px;
            int w = 0, h = 0;
            const std::string name = "splash_" + std::to_string(step) + ".png";
            if (hooks.frame(px, w, h) && w > 0 && h > 0) {
                for (uint32_t& p : px) p |= 0xFF000000u;
                const std::string err = PngIO::Write(name, px.data(), w, h);
                std::cout << "[AutoTest] " << (err.empty() ? "Saved: " + name : "Failed: " + name + " (" + err + ")") << std::endl;
            }
            int vk = 0;
            double hold = default_hold;
            if (auto k = keys.find(step); k != keys.end()) { vk = k->second.first; hold = k->second.second; }
            else if (step % key_every == 0) vk = 0x0D;   // Enter
            if (vk) { hooks.key(vk, true); const bool go = Sleep(hold); hooks.key(vk, false); if (!go) return; }
        }
        std::cout << "[AutoTest] Done" << std::endl;
        hooks.close();
    });
    return true;
}

void AutoTest::Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}
