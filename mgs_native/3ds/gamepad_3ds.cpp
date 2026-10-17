// The 3DS buttons as a gamepad (README.md §6.28). Each button is reported as the SDL gamepad button in the same
// position (A east, B south, X north, Y west; ZL/ZR as the triggers), so the [Controller] bindings in
// mgs_mobile.ini and the overlay's controller handling work unchanged. The circle pad is the left stick (analog
// steering), the New 3DS C-stick the right one. Select is Back (toggles the overlay). Polled once per frame
// from main_3ds.cpp on the game's thread (Poll), not on a thread of its own.
#include "gamepad.h"
#include "movement.h"
#include "settings.h"

#include <3ds.h>
#include <algorithm>
#include <cstring>

namespace {
enum : int { PAD_SOUTH = 0, PAD_EAST = 1, PAD_WEST = 2, PAD_NORTH = 3, PAD_BACK = 4, PAD_START = 6, PAD_RSTICK = 8,
             PAD_LB = 9, PAD_RB = 10, PAD_UP = 11, PAD_DOWN = 12, PAD_LEFT = 13, PAD_RIGHT = 14 };
constexpr int PAD_LT = PAD_LEFT_TRIGGER, PAD_RT = PAD_RIGHT_TRIGGER;
constexpr int S3E_LEFT = 0x09, S3E_UP = 0x0A, S3E_RIGHT = 0x0B, S3E_DOWN = 0x0C;   // S3E keys (gamepad.cpp)
}

Gamepad& Gamepad::Instance() {
    static Gamepad inst;
    return inst;
}

bool Gamepad::Reserved(int button) { return button == PAD_BACK || button == PAD_RSTICK; }

void Gamepad::Start(std::function<void(const bool keys[256])> on_keys, std::function<bool()> focused,
                    std::function<bool()> raw_mode, std::function<void(const Raw&)> on_raw, std::function<void()> on_back) {
    on_keys_ = std::move(on_keys);
    focused_ = std::move(focused);
    raw_mode_ = std::move(raw_mode);
    on_raw_ = std::move(on_raw);
    on_back_ = std::move(on_back);
    running_ = true;
}

void Gamepad::Stop() { running_ = false; }

void Gamepad::Run() {}

void Gamepad::Poll() {
    if (!running_) return;
    hidScanInput();
    const u32 held = hidKeysHeld();
    Raw raw{};
    const struct { u32 key; int button; } map[] = {
        {KEY_A, PAD_EAST}, {KEY_B, PAD_SOUTH}, {KEY_X, PAD_NORTH}, {KEY_Y, PAD_WEST}, {KEY_L, PAD_LB}, {KEY_R, PAD_RB},
        {KEY_ZL, PAD_LT}, {KEY_ZR, PAD_RT}, {KEY_SELECT, PAD_BACK}, {KEY_START, PAD_START},
        {KEY_DUP, PAD_UP}, {KEY_DDOWN, PAD_DOWN}, {KEY_DLEFT, PAD_LEFT}, {KEY_DRIGHT, PAD_RIGHT} };
    for (const auto& m : map) if (held & m.key) raw.buttons[m.button] = true;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    auto axis = [](int v) { return std::clamp(v / 150.0f, -1.0f, 1.0f); };
    raw.lx = axis(cp.dx); raw.ly = -axis(cp.dy);   // SDL: y grows downwards
    raw.rx = axis(cs.dx); raw.ry = -axis(cs.dy);

    static bool last_back = false, stick_held = false, last_keys[256]{};
    const bool back = raw.buttons[PAD_BACK];
    if (back && !last_back && on_back_) on_back_();
    last_back = back;

    bool keys[256]{};
    if (raw_mode_ && raw_mode_()) {
        // The overlay clicks with the south button (Xbox A, which is B here): on the 3DS, A clicks too.
        Raw overlay = raw;
        overlay.buttons[PAD_SOUTH] = raw.buttons[PAD_SOUTH] || raw.buttons[PAD_EAST];
        if (on_raw_) on_raw_(overlay);
        Movement::SetStick(Movement::PAD, 0, 0);
    } else {
        for (int b = 0; b < PAD_BUTTON_COUNT; ++b)
            if (raw.buttons[b] && !Reserved(b))
                for (uint8_t k : Settings::Instance().KeysForPadButton(b)) keys[k] = true;
        if (Settings::Instance().left_stick_dpad) {
            bool up, down, left, right;
            Movement::StickKeys(raw.lx, raw.ly, &stick_held, up, down, left, right);
            keys[S3E_UP] |= up; keys[S3E_DOWN] |= down; keys[S3E_LEFT] |= left; keys[S3E_RIGHT] |= right;
            Movement::SetStick(Movement::PAD, stick_held ? raw.lx : 0, stick_held ? raw.ly : 0);
        } else Movement::SetStick(Movement::PAD, 0, 0);
    }
    if (memcmp(keys, last_keys, sizeof(keys)) != 0) {
        memcpy(last_keys, keys, sizeof(keys));
        if (on_keys_) on_keys_(keys);
    }
}
