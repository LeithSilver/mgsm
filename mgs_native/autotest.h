#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// Scripted test run inside the exe (test_splash_progression.py sets it up; works on every platform):
//   MGS_AUTOTEST="steps,interval_s,key_every"  each step: optional resize, wait interval_s, save splash_<step>.png
//                                              (the last presented frame) in the working directory, then press a
//                                              key: Enter every key_every steps unless the key script says otherwise
//   MGS_KEY_SCRIPT="step:vk[/hold_s],..."      key (hex Windows VK code; + 0x100 = with Shift) for a step;
//                                              MGS_KEY_HOLD = default hold
//   MGS_RESIZE_SCRIPT="step:WxH,..."           window client size before a step's screenshot
// After the last step the window is closed.
namespace AutoTest {
struct Hooks {
    std::function<void(int vk, bool down)> key;                       // as if typed (any thread)
    std::function<bool(std::vector<uint32_t>& bgra, int& w, int& h)> frame;   // last presented frame, top-down BGRA
    std::function<void(int w, int h)> resize;                         // client size
    std::function<void()> close;
};
// Starts the script on its own thread if MGS_AUTOTEST is set; returns whether it did.
bool Start(Hooks hooks);
// Ends the script early if it is still running (the window closed) and waits for its thread.
void Stop();
}
