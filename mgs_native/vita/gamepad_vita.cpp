// The Vita's buttons as a gamepad (README.md §6.29). Each button is reported as the SDL gamepad button in the same
// position (Cross south, Circle east, Square west, Triangle north), so the [Controller] bindings in
// mgs_mobile.ini and the overlay's controller handling work unchanged. The sticks are the sticks; the left and
// right halves of the rear touch pad stand in for the triggers (a Vita 1000 has no L2 / R2). Select is Back
// (toggles the overlay). Polled once per frame from main_vita.cpp on the game's thread (Poll).
#include "gamepad.h"
#include "movement.h"
#include "overlay.h"
#include "settings.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace {
enum : int { PAD_SOUTH = 0, PAD_EAST = 1, PAD_WEST = 2, PAD_NORTH = 3, PAD_BACK = 4, PAD_START = 6, PAD_RSTICK = 8,
             PAD_LB = 9, PAD_RB = 10, PAD_UP = 11, PAD_DOWN = 12, PAD_LEFT = 13, PAD_RIGHT = 14 };
constexpr int PAD_LT = PAD_LEFT_TRIGGER, PAD_RT = PAD_RIGHT_TRIGGER;
constexpr int S3E_LEFT = 0x09, S3E_UP = 0x0A, S3E_RIGHT = 0x0B, S3E_DOWN = 0x0C;   // S3E keys (gamepad.cpp)
std::atomic<uint32_t> g_injected{0};          // Inject: buttons held until g_injected_until (process time, us)
std::atomic<uint64_t> g_injected_until{0};
}

void Gamepad::Inject(uint32_t buttons, int ms) {
    g_injected_until = sceKernelGetProcessTimeWide() + static_cast<uint64_t>(ms) * 1000;
    g_injected = buttons;
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
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    running_ = true;
}

void Gamepad::Stop() { running_ = false; }

void Gamepad::Run() {}

void Gamepad::Poll() {
    if (!running_) return;
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (g_injected && sceKernelGetProcessTimeWide() < g_injected_until) pad.buttons |= g_injected;
    Raw raw{};
    const struct { uint32_t key; int button; } map[] = {
        {SCE_CTRL_CROSS, PAD_SOUTH}, {SCE_CTRL_CIRCLE, PAD_EAST}, {SCE_CTRL_SQUARE, PAD_WEST}, {SCE_CTRL_TRIANGLE, PAD_NORTH},
        {SCE_CTRL_LTRIGGER, PAD_LB}, {SCE_CTRL_RTRIGGER, PAD_RB}, {SCE_CTRL_SELECT, PAD_BACK}, {SCE_CTRL_START, PAD_START},
        {SCE_CTRL_UP, PAD_UP}, {SCE_CTRL_DOWN, PAD_DOWN}, {SCE_CTRL_LEFT, PAD_LEFT}, {SCE_CTRL_RIGHT, PAD_RIGHT} };
    for (const auto& m : map) if (pad.buttons & m.key) raw.buttons[m.button] = true;
    SceTouchData back{};
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1) >= 0)
        for (SceUInt32 i = 0; i < back.reportNum; ++i)
            raw.buttons[back.report[i].x < 960 ? PAD_LT : PAD_RT] = true;   // (rear pad: 0..1919 wide)
    auto axis = [](uint8_t v) { return std::clamp((static_cast<int>(v) - 128) / 127.0f, -1.0f, 1.0f); };
    raw.lx = axis(pad.lx); raw.ly = axis(pad.ly);   // (y grows downwards, as SDL's)
    raw.rx = axis(pad.rx); raw.ry = axis(pad.ry);

    static bool last_back = false, stick_held = false, last_keys[256]{};
    const bool back_button = raw.buttons[PAD_BACK];
    if (back_button && !last_back && on_back_) on_back_();
    last_back = back_button;

    bool keys[256]{};
    if (raw_mode_ && raw_mode_()) {
        if (on_raw_) on_raw_(raw);
        // The front touch screen is the menu's mouse (0..1919 x 0..1087: twice the screen's pixels)
        static bool touching = false;
        SceTouchData front{};
        const bool now = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &front, 1) >= 0 && front.reportNum > 0;
        if (now) Overlay::Instance().OnMouseMove(front.report[0].x / 2.0f, front.report[0].y / 2.0f);
        if (now != touching) Overlay::Instance().OnMouseButton(0, now);
        touching = now;
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
