#include "gamepad.h"
#include "movement.h"
#include "settings.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr int KEY_LEFT = 0x09, KEY_UP = 0x0A, KEY_RIGHT = 0x0B, KEY_DOWN = 0x0C;   // S3E keys

constexpr int TRIGGER_PRESS = 16384;   // triggers count as buttons past half travel

// Debug: MGS_PAD_SCRIPT="ms:button+|-,..." attaches an SDL virtual gamepad and presses/releases its buttons
// at the given times (ms since start), e.g. "11000:south+,11100:south-". Buttons: south east west north
// start back lb rb up down left right; "lx"/"ly"/"lt"/"rt" set an axis: "12000:lx=-32767".
struct ScriptStep { uint64_t ms; int button; int axis; int value; };
std::vector<ScriptStep> ParsePadScript(const char* s) {
    static const struct { const char* name; int id; } kNames[] = {
        {"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST}, {"west", SDL_GAMEPAD_BUTTON_WEST},
        {"north", SDL_GAMEPAD_BUTTON_NORTH}, {"start", SDL_GAMEPAD_BUTTON_START}, {"back", SDL_GAMEPAD_BUTTON_BACK},
        {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT} };
    static const struct { const char* name; int id; } kAxes[] = {
        {"lx", SDL_GAMEPAD_AXIS_LEFTX}, {"ly", SDL_GAMEPAD_AXIS_LEFTY}, {"rx", SDL_GAMEPAD_AXIS_RIGHTX}, {"ry", SDL_GAMEPAD_AXIS_RIGHTY},
        {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER} };
    std::vector<ScriptStep> steps;
    std::string all(s);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos); if (end == std::string::npos) end = all.size();
        std::string item = all.substr(pos, end - pos); pos = end + 1;
        size_t colon = item.find(':'); if (colon == std::string::npos) continue;
        ScriptStep st{ std::stoull(item.substr(0, colon)), -1, -1, 0 };
        std::string act = item.substr(colon + 1);
        size_t eq = act.find('=');
        if (eq != std::string::npos) {
            for (auto& a : kAxes) if (act.substr(0, eq) == a.name) st.axis = a.id;
            st.value = std::stoi(act.substr(eq + 1));
        } else if (!act.empty()) {
            st.value = act.back() == '+' ? 1 : 0; act.pop_back();
            for (auto& n : kNames) if (act == n.name) st.button = n.id;
        }
        if (st.button >= 0 || st.axis >= 0) steps.push_back(st);
    }
    std::stable_sort(steps.begin(), steps.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.ms < b.ms; });
    return steps;
}
}

bool Gamepad::Reserved(int b) { return b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_RIGHT_STICK; }

Gamepad& Gamepad::Instance() {
    static Gamepad inst;
    return inst;
}

void Gamepad::Start(std::function<void(const bool keys[256])> on_keys, std::function<bool()> focused,
                    std::function<bool()> raw_mode, std::function<void(const Raw&)> on_raw, std::function<void()> on_back) {
    if (running_) return;
    on_keys_ = std::move(on_keys);
    focused_ = std::move(focused);
    raw_mode_ = std::move(raw_mode);
    on_raw_ = std::move(on_raw);
    on_back_ = std::move(on_back);
    running_ = true;
    thread_ = std::thread(&Gamepad::Run, this);
}

void Gamepad::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// main.cpp initialises SDL (with the gamepad subsystem) and pumps its events on the UI thread. This thread
// finds pads by enumeration instead of events (the event queue belongs to the UI thread) and refreshes their
// state itself with SDL_UpdateGamepads (the joystick API locks internally).
void Gamepad::Run() {
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        std::cerr << "[Pad] SDL gamepad subsystem not initialised (gamepad support off)" << std::endl;
        return;
    }
    std::vector<SDL_Gamepad*> pads;
    std::vector<ScriptStep> script;
    SDL_Joystick* vpad = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    if (const char* ps = getenv("MGS_PAD_SCRIPT")) {
        script = ParsePadScript(ps);
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "MGS virtual test pad";
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        vpad = id ? SDL_OpenJoystick(id) : nullptr;
        std::cout << "[Pad] Virtual test pad " << (vpad ? "attached" : "FAILED") << ", " << script.size() << " script steps" << std::endl;
    }
    bool stick_held = false;   // left stick out of its dead zone (hysteresis, Movement::StickKeys)
    bool last_keys[256]{};
    bool last_back = false;
    auto next_scan = std::chrono::steady_clock::time_point{};
    while (running_) {
        SDL_UpdateGamepads();
        if (std::chrono::steady_clock::now() >= next_scan) {   // hotplug: a few times a second
            next_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            for (size_t i = 0; i < pads.size();) {
                if (SDL_GamepadConnected(pads[i])) { ++i; continue; }
                std::cout << "[Pad] Disconnected: " << (SDL_GetGamepadName(pads[i]) ? SDL_GetGamepadName(pads[i]) : "gamepad") << std::endl;
                SDL_CloseGamepad(pads[i]);
                pads.erase(pads.begin() + i);
            }
            int n = 0;
            if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
                for (int i = 0; i < n; ++i) {
                    bool open = false;
                    for (SDL_Gamepad* pad : pads) if (SDL_GetGamepadID(pad) == ids[i]) open = true;
                    if (open) continue;
                    if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                        pads.push_back(pad);
                        std::cout << "[Pad] Connected: " << (SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "gamepad") << std::endl;
                    }
                }
                SDL_free(ids);
            }
        }
        if (vpad) {
            const uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            while (!script.empty() && script.front().ms <= ms) {
                const ScriptStep st = script.front(); script.erase(script.begin());
                if (st.button >= 0) SDL_SetJoystickVirtualButton(vpad, st.button, st.value != 0);
                else SDL_SetJoystickVirtualAxis(vpad, st.axis, static_cast<Sint16>(st.value));
            }
        }

        // Raw state across all pads (the test pad ignores window focus).
        Raw raw{};
        const bool active = vpad || !focused_ || focused_();
        int lx = 0, ly = 0, rx = 0, ry = 0;
        if (active) {
            for (SDL_Gamepad* pad : pads) {
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < PAD_LEFT_TRIGGER; ++b)
                    if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) raw.buttons[b] = true;
                if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_PRESS) raw.buttons[PAD_LEFT_TRIGGER] = true;
                if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_PRESS) raw.buttons[PAD_RIGHT_TRIGGER] = true;
                int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
                if (std::abs(x) > std::abs(lx)) lx = x;   // strongest stick wins across pads
                if (std::abs(y) > std::abs(ly)) ly = y;
                const int r = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
                if (std::abs(r) > std::abs(ry)) ry = r;
                const int rh = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
                if (std::abs(rh) > std::abs(rx)) rx = rh;
            }
        }
        raw.lx = lx / 32767.0f; raw.ly = ly / 32767.0f; raw.rx = rx / 32767.0f; raw.ry = ry / 32767.0f;

        const bool back = raw.buttons[SDL_GAMEPAD_BUTTON_BACK] || raw.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK];   // reserved: toggle the overlay
        if (back && !last_back && on_back_) on_back_();
        last_back = back;

        bool keys[256]{};
        if (raw_mode_ && raw_mode_()) {
            if (on_raw_) on_raw_(raw);
        } else {
            for (int b = 0; b < PAD_BUTTON_COUNT; ++b)
                if (raw.buttons[b] && !Reserved(b))
                    for (uint8_t k : Settings::Instance().KeysForPadButton(b)) keys[k] = true;
            if (Settings::Instance().left_stick_dpad) {   // the direction keys, plus the stick itself for analog movement
                bool up, down, left, right;
                Movement::StickKeys(raw.lx, raw.ly, &stick_held, up, down, left, right);
                keys[KEY_UP] |= up; keys[KEY_DOWN] |= down; keys[KEY_LEFT] |= left; keys[KEY_RIGHT] |= right;
                Movement::SetStick(Movement::PAD, stick_held ? raw.lx : 0, stick_held ? raw.ly : 0);
            } else Movement::SetStick(Movement::PAD, 0, 0);
        }
        if (raw_mode_ && raw_mode_()) Movement::SetStick(Movement::PAD, 0, 0);   // (the overlay has the pad)
        if (memcmp(keys, last_keys, sizeof(keys)) != 0) {
            memcpy(last_keys, keys, sizeof(keys));
            if (on_keys_) on_keys_(keys);
        }
        SDL_Delay(4);
    }
    for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
    if (vpad) SDL_CloseJoystick(vpad);
}
