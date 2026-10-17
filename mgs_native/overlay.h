#pragma once

#include "gamepad.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct ImGuiIO;

// F1 settings overlay (Dear ImGui), drawn into the game's frame on the emu thread just before it is read
// back (GLES::Present). Input arrives from the UI thread (mouse/keyboard, in frame pixel coordinates) and
// the gamepad thread (raw buttons: menu navigation and binding capture) through a locked queue.
class Overlay {
public:
    static Overlay& Instance();

    bool IsOpen() const { return open_; }
    void SetOpen(bool open);              // UI thread (main releases the game's keys first)

    // UI thread input, only routed here while open.
    void OnMouseMove(float x, float y);   // frame coordinates
    void OnMouseButton(int button, bool down);
    void OnMouseWheel(float delta);
    void OnKey(int vk, bool down);
    void OnChar(unsigned c);
    // Keyboard binding capture: while true the next key press goes to OnCaptureKey (Esc cancels).
    bool CapturingKey() const { return capture_kind_ == 1; }
    void OnCaptureKey(int vk);

    // Gamepad thread: raw state while open (navigation, button capture).
    void OnPad(const Gamepad::Raw& raw);

    // Emu thread, from GLES::Present: draw the overlay. While closed, only the debug-mode warning list
    // (recent AddWarning messages) is drawn, if there is any.
    void Render(int fb_w, int fb_h);
    // Any thread: a warning to show on screen for a few seconds in debug mode (and in the General tab).
    void AddWarning(const std::string& text);
    // Any thread: a short message shown on screen in any mode (e.g. "Screenshot saved").
    void AddNotice(const std::string& text);
    // Called (UI thread via main) when the user picks "Photo mode" in the menu.
    void SetPhotoModeCallback(std::function<void()> cb) { photo_ = std::move(cb); }

    // Called (emu thread) after a setting changed; main applies it to the subsystems and saves.
    void SetApplyCallback(std::function<void()> cb) { apply_ = std::move(cb); }
    // Called (emu thread) when the user asks to close the overlay from inside it.
    void SetCloseCallback(std::function<void()> cb) { close_ = std::move(cb); }
    // Level select tab: whether a room change is possible now (gameplay), and the request itself.
    void SetLevelSelect(std::function<bool()> can_load, std::function<void(const std::string&, const std::string&)> load) {
        can_load_ = std::move(can_load); load_room_ = std::move(load);
    }

private:
    Overlay() = default;
    void Draw();
    void DrawBindings(bool controller);
    void DrawGeneral(bool& changed);
    void DrawTouch(bool& changed);
    bool touch_tab_ = false;   // the Touch tab was showing last frame (preview, see-through window)
    int press_frame_[3] = {-100, -100, -100};   // ImGui frame of the last press per mouse button (taps)
    void PadCursor(ImGuiIO& io, int fb_w, int fb_h);
    float cursor_x_ = -1.0f, cursor_y_ = -1.0f;   // overlay cursor (frame pixels), shared by mouse and controller
    bool pad_cursor_ = false;                       // the controller moved it last (ImGui draws the cursor)
    void DrawInventory(bool& changed);
    void DrawWarnings();   // the on-screen list while closed
    bool WarningsVisible();

    struct Warning { std::string text; std::chrono::steady_clock::time_point time; bool notice = false; };
    std::function<void()> photo_;
    void DrawPhotoHint();
    std::mutex warnings_mutex_;
    std::vector<Warning> warnings_;

    struct Event { int type; int a; int b; float x; float y; };
    std::mutex mutex_;
    std::vector<Event> events_;
    Gamepad::Raw pad_{}, pad_prev_{};
    bool pad_valid_ = false;

    std::atomic<bool> open_{false};
    std::atomic<int> capture_kind_{0};    // 0 none, 1 keyboard, 2 controller
    std::atomic<int> capture_action_{-1};
    std::atomic<int> captured_{-1};       // VK or pad button waiting to be applied (emu thread)
    bool initialised_ = false;
    std::function<void()> apply_, close_;
    std::function<bool()> can_load_;
    std::function<void(const std::string&, const std::string&)> load_room_;   // (room, neighbour to arrive from)
    int level_selected_ = -1;
    bool DrawLevelSelect();   // true: a room was chosen (close the overlay)
};
