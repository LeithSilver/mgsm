#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

struct ImDrawList;

// On-screen touch controls (README.md §6.21): the D-pad with its centre OK button, the A and B buttons and
// the two soft keys, drawn from the artwork outlines (touch_shapes.h, tools/make_touch_shapes.py), plus a small menu
// button at the top middle that opens the F1 overlay while the game is in a menu or the codec. Placement,
// size, opacity and the mode (off / auto / on) are settings (F1 > Touch). Auto: shown once the screen is
// touched, hidden again when a controller or keyboard is used.
//
// Coordinates are frame pixels (the overlay's ImGui space). Fingers come from the UI thread, drawing happens
// on the emu thread (overlay pass): the shared state is atomic.
class TouchControls {
public:
    static TouchControls& Instance();
    enum Control { DPAD, A, LSK, RSK, B, COUNT };
    static const char* Name(int c);   // "dpad", "a", "lsk", "rsk", "b" (ini keys)
    static const char* Label(int c);
    static void DefaultPlace(int c, float& x, float& y, float& size);   // centre (fractions of width/height), height fraction

    // UI thread. The frame size is the one the fingers were mapped into. The key callback gets the phone keys
    // the touch controls hold (256 flags), the menu callback runs on a tap of the menu button.
    void SetKeyCallback(std::function<void(const bool keys[256])> cb) { keys_cb_ = std::move(cb); }
    void SetMenuCallback(std::function<void()> cb) { menu_cb_ = std::move(cb); }
    void FingerDown(int64_t id, float x, float y, int fw, int fh);
    void FingerMove(int64_t id, float x, float y, int fw, int fh);
    void FingerUp(int64_t id, float x, float y, int fw, int fh);
    void ReleaseAll();
    void OtherInput();   // a controller button / key was used: Auto mode hides the controls
    // Whether touches are taken (mode on or auto) and whether the controls are drawn.
    bool Enabled() const;
    bool Visible() const;
    // Menu button shown (the game is in a menu / the codec): set every frame by main.
    void SetMenuAvailable(bool on) { menu_available_ = on; }

    // Emu thread: draw into the overlay frame. preview: the F1 Touch tab is open (drawn even if hidden).
    void Draw(ImDrawList* dl, int fw, int fh, bool preview);

private:
    TouchControls() = default;
    struct Rect { float x0, y0, x1, y1; bool Contains(float x, float y, float margin = 0) const {
        return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin; } };
    static Rect Place(int c, int fw, int fh, float* scale = nullptr);   // screen rect of a control
    static Rect MenuRect(int fw, int fh);
    // Direction / centre bits for a finger; with vx / vy also its direction as a unit vector (0, 0 if none).
    uint32_t DpadBits(float x, float y, int fw, int fh, float* vx = nullptr, float* vy = nullptr) const;
    void Update();   // pressed bits from the fingers -> keys; the D-pad finger's direction -> Movement

    enum Bit : uint32_t { B_UP = 1, B_RIGHT = 2, B_DOWN = 4, B_LEFT = 8, B_OK = 16, B_A = 32, B_LSK = 64, B_RSK = 128, B_MENU = 256, B_B = 512 };
    static uint32_t ButtonBit(int c);   // a button control's bit (0 for the D-pad)
    struct Finger { int control; uint32_t bits; float vx = 0, vy = 0; };   // control -1: the menu button; -2: nothing
    std::recursive_mutex mutex_;                     // fingers_ (UI thread, and OtherInput from the gamepad thread)
    std::map<int64_t, Finger> fingers_;
    std::atomic<uint32_t> pressed_{0};
    std::atomic<bool> shown_{true};                  // Auto mode: touched since the last controller / key use
    std::atomic<bool> menu_available_{false};
    std::function<void(const bool[256])> keys_cb_;
    std::function<void()> menu_cb_;
};
