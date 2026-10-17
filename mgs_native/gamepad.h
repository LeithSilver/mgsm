#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

// SDL3 gamepad input on its own thread. Every poll it reads all connected pads into raw button states
// (SDL_GamepadButton ids plus the triggers as pseudo-buttons 26/27, see settings.h) and both sticks.
//  - Normally they're mapped to phone (S3E) keys through Settings (bindings + left stick as d-pad) and
//    reported with on_keys.
//  - While the overlay wants them (raw_mode() true) the phone keys are all released and the raw state is
//    reported with on_raw instead (overlay cursor, button capture for rebinding).
//  - Back/Select and R3 (right stick click) are reserved: a press calls on_back (toggles the overlay).
// Buttons are matched by position (south/east/west/north), so Xbox, PlayStation etc. behave alike.
class Gamepad {
public:
    struct Raw { bool buttons[28]; float lx, ly, rx, ry; };   // sticks -1..1
    static bool Reserved(int button);   // Back and R3: never bound, never sent to the game
    static Gamepad& Instance();
    void Start(std::function<void(const bool keys[256])> on_keys, std::function<bool()> focused,
               std::function<bool()> raw_mode, std::function<void(const Raw&)> on_raw, std::function<void()> on_back);
    void Stop();
#if defined(__3DS__) || defined(__vita__)
    // 3ds/gamepad_3ds.cpp, vita/gamepad_vita.cpp: reads the buttons (once per frame, on the game's thread; no
    // thread of its own)
    void Poll();
#ifdef __vita__
    static void Inject(uint32_t buttons, int ms);   // test input (main_vita.cpp's command port)
#endif
#endif

private:
    Gamepad() = default;
    void Run();

    std::function<void(const bool keys[256])> on_keys_;
    std::function<bool()> focused_, raw_mode_;
    std::function<void(const Raw&)> on_raw_;
    std::function<void()> on_back_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};
