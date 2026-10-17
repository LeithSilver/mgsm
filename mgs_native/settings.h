#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "keys.h"   // VK codes, VKName

// User settings, persisted in mgs_mobile.ini next to the executable. Read from several threads (UI, emu,
// gamepad): plain flags are atomics, bindings are guarded by a mutex and handed out as copies.
//
// Bindings map each phone (S3E) key to any number of PC virtual keys and SDL gamepad buttons.
struct PhoneAction {
    const char* id;      // ini key
    const char* label;   // shown in the overlay
    uint8_t key;         // S3E key code
};
const std::vector<PhoneAction>& PhoneActions();

class Settings {
public:
    static Settings& Instance();

    void Load();                  // missing file / keys -> defaults
    void Save();                  // writes the whole file
    void ResetKeyboard();         // default keyboard bindings
    void ResetController();       // default controller bindings
    void ResetTouchLayout();      // default touch control placement

    std::atomic<bool> fullscreen{false};
    std::atomic<bool> arbitrary_resolution{true};   // aspect-following screen + full-bleed 3D view
    std::atomic<bool> texture_filtering{false};
    std::atomic<bool> custom_textures{true};   // textures\ replacements (textures.cpp)
    std::atomic<bool> draw_all_geometry{false};    // ignore the game's per-camera visibility sets
    std::atomic<bool> custom_player_model{false};  // customModel/snake.mgsm in place of Snake (custommodel.cpp)
    std::atomic<bool> software_renderer{false};    // IwGx's own software rasteriser instead of GL ES (read at startup)
    std::atomic<int> msaa{0};                      // MSAA samples: 0 off, 2, 4, 8
    std::atomic<int> supersample{1};                // render-scale factor 1..3
    std::atomic<bool> native_resolution{false};     // GL: render at the guest screen size, nearest upscale
    std::atomic<int> hide_soft_keys{0};            // 0 off, 1 smart (gameplay only), 2 all soft-key labels
    std::atomic<int> fov_percent{100};              // 3D horizontal/vertical FOV scale (100 = game default)
    std::atomic<bool> left_stick_dpad{true};
    std::atomic<bool> diagonal_movement{false};      // 8-way movement (movement.cpp)
    std::atomic<bool> analog_movement{false};        // left stick / touch D-pad: any angle
    std::atomic<bool> skip_intro{false};            // boot straight past the Konami / Kojima Productions / Ideaworks3D logos
    std::atomic<bool> pause_on_focus_loss{false};   // tell the game the "phone" was interrupted (OS pause) while unfocused
    // Debug mode: level select, on-screen [GLES] warnings and the verbose console logs. Unless the ini sets it,
    // main.cpp defaults it on in the repo layout and off in a distribution (a game folder next to the exe).
    std::atomic<bool> debug_mode{false};
    bool debug_mode_in_ini = false;
    // Cheats (F1 > Inventory, debug mode only; inactive while debug mode is off).
    std::atomic<bool> cheat_infinite_health{false};
    std::atomic<bool> cheat_infinite_ammo{false};
    // Original debug commands (F1 > General, debug mode only).
    std::atomic<bool> dbg_show_fps{false};          // IwGx frame-rate counter
    std::atomic<bool> dbg_lua_debug_mode{false};    // Lua InDebugMode(): Assert / Trace messages (read once at startup)
    std::atomic<bool> dbg_frame_rate_test{false};   // Lua IsFrameRateTest(): new game starts at the test spawn

    // Touch controls (touchcontrols.cpp): 0 off, 1 auto (shown once the screen is touched), 2 on; opacity in
    // percent; per control (TouchControls::Control) the centre as fractions of the frame and the height as a
    // fraction of the frame height.
#ifdef __ANDROID__
    std::atomic<int> touch_mode{1};
#else
    std::atomic<int> touch_mode{0};
#endif
    std::atomic<int> touch_opacity{60};
    static constexpr int kTouchControls = 5;
    std::atomic<float> touch_x[kTouchControls], touch_y[kTouchControls], touch_size[kTouchControls];

    // Window (client area) size and position, saved when the user resizes/moves it.
    std::atomic<bool> window_saved{false};
    std::atomic<int> window_x{0}, window_y{0}, window_w{0}, window_h{0};

    // Bindings, indexed like PhoneActions().
    std::vector<std::vector<int>> KeyboardBindings();
    std::vector<std::vector<int>> ControllerBindings();
    void SetKeyboardBindings(const std::vector<std::vector<int>>& b);
    void SetControllerBindings(const std::vector<std::vector<int>>& b);
    // Fast lookups (rebuilt on change): S3E keys for a PC key / for a gamepad button.
    std::vector<uint8_t> KeysForVK(int vk);
    std::vector<uint8_t> KeysForPadButton(int button);

    std::string Path() const { return path_; }

private:
    Settings();
    void RebuildLookups();   // mutex_ held

    std::mutex mutex_;
    std::string path_;
    std::vector<std::vector<int>> kb_, pad_;
    std::vector<std::vector<uint8_t>> vk_lookup_;    // 256 entries
    std::vector<std::vector<uint8_t>> pad_lookup_;   // per SDL_GamepadButton
};

// Display names for the overlay.
std::string PadButtonName(int button);
// SDL_GamepadButton ids 0..25 (SDL 3.4), plus the triggers as pseudo-buttons (pressed past half travel).
constexpr int PAD_LEFT_TRIGGER = 26, PAD_RIGHT_TRIGGER = 27, PAD_BUTTON_COUNT = 28;

// Verbose console logging ([VFS] opens, [First] imports, [Config] keys, GL queries): debug mode only.
inline bool VerboseLog() { return Settings::Instance().debug_mode; }
