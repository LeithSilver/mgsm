#pragma once

#include "gamepad.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Photo mode (debug mode, F5): the game clock stops (S3ERuntime freezes GameMs; F6 steps one update), the
// world keeps rendering, and a free camera replaces the game's view matrix (hook on IwGxSetViewMatrix for the
// main 3D view, README §6.18). Input comes from the UI thread (keys, mouse look) and the gamepad thread; the
// camera moves on the emu thread once per frame (Update) and is handed to the hook (ViewMatrix).
//
// Camera: IwGx CIwMat = 3x3 rotation (fixed point, 4096 = 1) + translation. The rows are the camera's right,
// screen-y and forward axes in world space and t is its position (view = axes . (world - t)); the world is y-up.
class PhotoMode {
public:
    static PhotoMode& Instance();

    bool Active() const { return active_; }
    void SetActive(bool on);   // any thread; the camera starts from the game's own view

    // UI thread input while active.
    void OnKey(int vk, bool down);
    void OnMouseLook(int dx, int dy);     // pixels moved with a mouse button held
    void OnMouseWheel(float notches);     // movement speed
    void ReleaseAll();
    // Gamepad thread input while active (raw state).
    void OnPad(const Gamepad::Raw& raw);

    // Emu thread: once per frame, moves the camera from the input (real time, not the frozen game clock).
    void Update();
    // Emu thread, from the IwGxSetViewMatrix hook: `game` = the matrix the game is setting (12 int32). Returns
    // false while photo mode is off; else fills `out` with the free camera (initialised from `game` on first use).
    bool ViewMatrix(const int32_t game[12], int32_t out[12]);
    void ResetToGameCamera() { reset_ = true; }

    float Speed() const { return speed_; }
    float Fov() const { return fov_; }            // % of the normal field of view (zoom: lower = narrower)
    bool HudHidden() const { return hud_hidden_; }

private:
    PhotoMode() = default;
    void Orthonormalise();

    std::atomic<bool> active_{false}, reset_{false};
    std::mutex mutex_;
    bool keys_[256]{};
    int mouse_dx_ = 0, mouse_dy_ = 0;
    Gamepad::Raw pad_{};
    bool pad_valid_ = false;
    std::atomic<float> speed_{4000.0f};   // world units per second
    std::atomic<float> fov_{100.0f};
    std::atomic<bool> hud_hidden_{true};

    // Camera (emu thread): position and axes in world space.
    bool have_camera_ = false;
    double pos_[3]{}, right_[3]{}, down_[3]{}, fwd_[3]{};
    double vert_[3]{0, -1, 0};   // world down (the world is y-up); yaw turns around it
    double handed_ = 1.0;       // down = handed * fwd x right (keeps the game's handedness)
    std::chrono::steady_clock::time_point last_{};
};
