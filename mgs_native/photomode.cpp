#include "photomode.h"
#include "gles.h"
#include "runtime.h"

#include "keys.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include "settings.h"

namespace {
double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
}
void Normalise(double v[3]) {
    const double l = std::sqrt(Dot(v, v));
    if (l > 1e-9) for (int i = 0; i < 3; ++i) v[i] /= l;
}
// Rotates v about the unit axis k by angle a (Rodrigues).
void Rotate(double v[3], const double k[3], double a) {
    double kxv[3];
    Cross(k, v, kxv);
    const double c = std::cos(a), s = std::sin(a), kd = Dot(k, v) * (1.0 - c);
    for (int i = 0; i < 3; ++i) v[i] = v[i] * c + kxv[i] * s + k[i] * kd;
}
float Stick(float v) {   // dead zone + squared response
    const float a = std::fabs(v);
    if (a < 0.15f) return 0.0f;
    const float t = (a - 0.15f) / 0.85f;
    return v < 0 ? -t * t : t * t;
}
// SDL button ids (gamepad.h / settings.h)
constexpr int B_SOUTH = 0, B_EAST = 1, B_WEST = 2, B_NORTH = 3, B_LB = 9, B_RB = 10, B_DUP = 11, B_DDOWN = 12,
              B_DLEFT = 13, B_LT = 26, B_RT = 27;
}

PhotoMode& PhotoMode::Instance() {
    static PhotoMode inst;
    return inst;
}

void PhotoMode::SetActive(bool on) {
    if (on && !active_) { reset_ = true; fov_ = 100.0f; }   // the emu thread re-captures the game's view at its next set
    if (!on) ReleaseAll();
    active_ = on;
}

void PhotoMode::OnKey(int vk, bool down) {
    std::lock_guard<std::mutex> l(mutex_);
    if (vk >= 0 && vk < 256) keys_[vk] = down;
    if (down && vk == 'R') { reset_ = true; fov_ = 100.0f; }
    if (down && vk == 'H') hud_hidden_ = !hud_hidden_;
}

void PhotoMode::OnMouseLook(int dx, int dy) {
    std::lock_guard<std::mutex> l(mutex_);
    mouse_dx_ += dx; mouse_dy_ += dy;
}

void PhotoMode::OnMouseWheel(float notches) {
    speed_ = (std::max)(250.0f, (std::min)(64000.0f, speed_ * std::pow(1.25f, notches)));
}

void PhotoMode::ReleaseAll() {
    std::lock_guard<std::mutex> l(mutex_);
    std::fill(std::begin(keys_), std::end(keys_), false);
    mouse_dx_ = mouse_dy_ = 0;
    pad_valid_ = false;
}

// Gamepad: left stick moves, right stick looks, LT / RT down / up, LB held = fast, RB = step one frame,
// Y = screenshot, X = back to the game's camera (and FOV), B = leave photo mode, d-pad up / down = zoom,
// d-pad left = show / hide the HUD. Back / R3 still open the F1 menu.
void PhotoMode::OnPad(const Gamepad::Raw& raw) {
    Gamepad::Raw prev;
    {
        std::lock_guard<std::mutex> l(mutex_);
        prev = pad_valid_ ? pad_ : raw;
        pad_ = raw;
        pad_valid_ = true;
    }
    auto pressed = [&](int b) { return raw.buttons[b] && !prev.buttons[b]; };
    if (pressed(B_RB)) S3ERuntime::Instance().StepFrame();
    if (pressed(B_NORTH)) GLES::Instance().RequestScreenshot();
    if (pressed(B_WEST)) { reset_ = true; fov_ = 100.0f; }
    if (pressed(B_DLEFT)) hud_hidden_ = !hud_hidden_;
    if (pressed(B_EAST)) SetActive(false);
}

void PhotoMode::Orthonormalise() {
    Normalise(fwd_);
    const double d = Dot(right_, fwd_);
    for (int i = 0; i < 3; ++i) right_[i] -= d * fwd_[i];
    Normalise(right_);
    Cross(fwd_, right_, down_);
    for (double& v : down_) v *= handed_;
}

void PhotoMode::Update() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = last_.time_since_epoch().count() ? (std::min)(0.1, std::chrono::duration<double>(now - last_).count()) : 0.0;
    last_ = now;
    if (!active_ || !have_camera_) return;
    bool k[256];
    int mdx, mdy;
    Gamepad::Raw pad;
    bool pad_ok;
    {
        std::lock_guard<std::mutex> l(mutex_);
        std::copy(std::begin(keys_), std::end(keys_), k);
        mdx = mouse_dx_; mdy = mouse_dy_; mouse_dx_ = mouse_dy_ = 0;
        pad = pad_; pad_ok = pad_valid_;
    }
    // Movement in camera space: x right, y up (screen), z forward.
    double mx = (k['D'] ? 1 : 0) - (k['A'] ? 1 : 0);
    double my = (k['E'] || k[VK_SPACE] ? 1 : 0) - (k['Q'] || k['C'] ? 1 : 0);
    double mz = (k['W'] ? 1 : 0) - (k['S'] ? 1 : 0);
    double yaw = (k[VK_RIGHT] ? 1 : 0) - (k[VK_LEFT] ? 1 : 0);   // + = turn right
    double pitch = (k[VK_UP] ? 1 : 0) - (k[VK_DOWN] ? 1 : 0);    // + = look up
    double fast = k[VK_SHIFT] ? 4.0 : (k[VK_CONTROL] ? 0.25 : 1.0);
    double zoom = (k['X'] ? 1 : 0) - (k['Z'] ? 1 : 0);   // + = wider
    if (pad_ok) {
        mx += Stick(pad.lx); mz -= Stick(pad.ly);
        my += (pad.buttons[B_RT] ? 1 : 0) - (pad.buttons[B_LT] ? 1 : 0);
        yaw += Stick(pad.rx); pitch -= Stick(pad.ry);
        if (pad.buttons[B_LB]) fast = 4.0;
        zoom += (pad.buttons[B_DDOWN] ? 1 : 0) - (pad.buttons[B_DUP] ? 1 : 0);
    }
    const double turn = 1.2 * dt;   // rad/s at full deflection
    const double a_yaw = yaw * turn + mdx * 0.004, a_pitch = pitch * turn - mdy * 0.004;
    // The rotation senses are checked against the current axes rather than assumed: turning right moves forward
    // towards the right axis, looking up moves it towards world up (-vert_).
    if (a_yaw != 0.0) {
        double f2[3] = { fwd_[0], fwd_[1], fwd_[2] };
        Rotate(f2, vert_, a_yaw);
        const double sense = (Dot(f2, right_) > 0) == (a_yaw > 0) ? 1.0 : -1.0;
        Rotate(right_, vert_, a_yaw * sense);
        Rotate(fwd_, vert_, a_yaw * sense);
    }
    if (a_pitch != 0.0) {
        double f2[3] = { fwd_[0], fwd_[1], fwd_[2] };
        Rotate(f2, right_, a_pitch);
        if ((Dot(f2, vert_) < Dot(fwd_, vert_)) != (a_pitch > 0)) { std::copy(fwd_, fwd_ + 3, f2); Rotate(f2, right_, -a_pitch); }
        if (std::fabs(Dot(f2, vert_)) < 0.995) std::copy(f2, f2 + 3, fwd_);   // stop short of straight up / down
    }
    if (zoom != 0.0) fov_ = static_cast<float>((std::max)(15.0, (std::min)(300.0, fov_ * std::pow(1.6, zoom * dt))));
    Orthonormalise();
    const double step = speed_ * fast * dt;
    for (int i = 0; i < 3; ++i) pos_[i] += (right_[i] * mx - vert_[i] * my + fwd_[i] * mz) * step;
}

bool PhotoMode::ViewMatrix(const int32_t game[12], int32_t out[12]) {
    if (!active_) return false;
    if (!have_camera_ || reset_) {
        reset_ = false;
        for (int i = 0; i < 3; ++i) {
            right_[i] = game[0 + i] / 4096.0;   // rows: right, screen y, forward
            down_[i] = game[3 + i] / 4096.0;
            fwd_[i] = game[6 + i] / 4096.0;
            pos_[i] = game[9 + i];
        }
        double c[3];
        Cross(fwd_, right_, c);
        handed_ = Dot(c, down_) < 0 ? -1.0 : 1.0;
        const int axis = 1;   // this game's world is y-up (floor near y = 0, room cameras above it)
        have_camera_ = true;
        if (VerboseLog()) {
            std::cout << "[Photo] Game camera: pos " << pos_[0] << " " << pos_[1] << " " << pos_[2] << ", right " << right_[0] << " "
                      << right_[1] << " " << right_[2] << ", down " << down_[0] << " " << down_[1] << " " << down_[2] << ", fwd "
                      << fwd_[0] << " " << fwd_[1] << " " << fwd_[2] << ", vertical axis " << axis << std::endl;
        }
    }
    for (int i = 0; i < 3; ++i) {
        out[0 + i] = static_cast<int32_t>(std::lround(right_[i] * 4096.0));
        out[3 + i] = static_cast<int32_t>(std::lround(down_[i] * 4096.0));
        out[6 + i] = static_cast<int32_t>(std::lround(fwd_[i] * 4096.0));
        out[9 + i] = static_cast<int32_t>(std::lround(pos_[i]));
    }
    return true;
}
