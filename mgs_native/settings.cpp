#include "settings.h"
#include "keys.h"
#include "platform.h"
#include "touchcontrols.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

// SDL_GamepadButton values (kept here so this file needn't include SDL)
enum { B_SOUTH, B_EAST, B_WEST, B_NORTH, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LSHOULDER, B_RSHOULDER,
       B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT };

const std::vector<PhoneAction>& PhoneActions() {
    static const std::vector<PhoneAction> actions = {
        {"up",       "Up",                          0x0A},
        {"down",     "Down",                        0x0C},
        {"left",     "Left",                        0x09},
        {"right",    "Right",                       0x0B},
        {"a",        "A  (Action / OK)",            0xC8},
        {"b",        "B  (First-person view)",      0xC9},
        {"ok",       "Center / OK",                 0x4E},
        {"lsk",      "Left soft key  (Pause menu)", 0x49},
        {"rsk",      "Right soft key  (Equipment)", 0x48},
        {"star",     "Key *  (Camera)",             0x4D},
        {"hash",     "Key #",                       0x4C},
        {"clear",    "Clear",                       0x4F},
        {"key0", "Key 0", 0x0D}, {"key1", "Key 1", 0x0E}, {"key2", "Key 2", 0x0F}, {"key3", "Key 3", 0x10},
        {"key4", "Key 4", 0x11}, {"key5", "Key 5", 0x12}, {"key6", "Key 6", 0x13}, {"key7", "Key 7", 0x14},
        {"key8", "Key 8", 0x15}, {"key9", "Key 9", 0x16},
        {"volup",    "Volume up",                   0x50},
        {"voldown",  "Volume down",                 0x51},
    };
    return actions;
}

static int ActionIndex(const char* id) {
    const auto& a = PhoneActions();
    for (size_t i = 0; i < a.size(); ++i) if (strcmp(a[i].id, id) == 0) return static_cast<int>(i);
    return -1;
}

Settings& Settings::Instance() {
    static Settings inst;
    return inst;
}

Settings::Settings() {
    path_ = Platform::U8String(Platform::ExeDir() / "mgs_mobile.ini");
    ResetKeyboard();
    ResetController();
    ResetTouchLayout();
}

void Settings::ResetTouchLayout() {
    for (int c = 0; c < kTouchControls; ++c) {
        float x, y, size;
        TouchControls::DefaultPlace(c, x, y, size);
        touch_x[c] = x; touch_y[c] = y; touch_size[c] = size;
    }
}

static float ParseClamped(const std::string& v, float lo, float hi) { return (std::max)(lo, (std::min)(hi, static_cast<float>(atof(v.c_str())))); }

void Settings::ResetKeyboard() {
    std::lock_guard<std::mutex> lock(mutex_);
    kb_.assign(PhoneActions().size(), {});
    auto set = [&](const char* id, std::vector<int> vks) { kb_[ActionIndex(id)] = std::move(vks); };
    set("up", {VK_UP}); set("down", {VK_DOWN}); set("left", {VK_LEFT}); set("right", {VK_RIGHT});
    set("a", {'Z'}); set("b", {'C'});
    set("ok", {VK_RETURN, VK_SPACE, 'J'});
    set("lsk", {'A'}); set("rsk", {'X', VK_ESCAPE, 'K', VK_F2});
    set("star", {VK_MULTIPLY}); set("hash", {VK_TAB, 'L'}); set("clear", {VK_BACK});
    for (int d = 0; d <= 9; ++d) {
        char id[8]; snprintf(id, sizeof(id), "key%d", d);
        set(id, {'0' + d, VK_NUMPAD0 + d});
    }
    set("volup", {VK_PRIOR}); set("voldown", {VK_NEXT});
    RebuildLookups();
}

void Settings::ResetController() {
    std::lock_guard<std::mutex> lock(mutex_);
    pad_.assign(PhoneActions().size(), {});
    auto set = [&](const char* id, std::vector<int> b) { pad_[ActionIndex(id)] = std::move(b); };
    set("up", {B_DUP}); set("down", {B_DDOWN}); set("left", {B_DLEFT}); set("right", {B_DRIGHT});
    set("a", {B_SOUTH}); set("b", {B_NORTH});
    set("rsk", {B_EAST, B_RSHOULDER});
    set("lsk", {B_START, B_WEST, B_LSHOULDER});
    left_stick_dpad = true;
    RebuildLookups();
}

void Settings::RebuildLookups() {
    vk_lookup_.assign(256, {});
    pad_lookup_.assign(PAD_BUTTON_COUNT, {});
    const auto& a = PhoneActions();
    for (size_t i = 0; i < a.size() && i < kb_.size(); ++i)
        for (int vk : kb_[i]) if (vk > 0 && vk < 256) vk_lookup_[vk].push_back(a[i].key);
    for (size_t i = 0; i < a.size() && i < pad_.size(); ++i)
        for (int b : pad_[i]) if (b >= 0 && b < PAD_BUTTON_COUNT) pad_lookup_[b].push_back(a[i].key);
}

std::vector<std::vector<int>> Settings::KeyboardBindings() { std::lock_guard<std::mutex> l(mutex_); return kb_; }
std::vector<std::vector<int>> Settings::ControllerBindings() { std::lock_guard<std::mutex> l(mutex_); return pad_; }
void Settings::SetKeyboardBindings(const std::vector<std::vector<int>>& b) { std::lock_guard<std::mutex> l(mutex_); kb_ = b; kb_.resize(PhoneActions().size()); RebuildLookups(); }
void Settings::SetControllerBindings(const std::vector<std::vector<int>>& b) { std::lock_guard<std::mutex> l(mutex_); pad_ = b; pad_.resize(PhoneActions().size()); RebuildLookups(); }
std::vector<uint8_t> Settings::KeysForVK(int vk) {
    std::lock_guard<std::mutex> l(mutex_);
    return (vk >= 0 && vk < 256) ? vk_lookup_[vk] : std::vector<uint8_t>{};
}
std::vector<uint8_t> Settings::KeysForPadButton(int b) {
    std::lock_guard<std::mutex> l(mutex_);
    return (b >= 0 && b < PAD_BUTTON_COUNT) ? pad_lookup_[b] : std::vector<uint8_t>{};
}

static std::vector<int> ParseList(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(atoi(item.c_str()));
    return out;
}

void Settings::Load() {
    std::ifstream f(Platform::U8Path(path_));
    if (!f) { std::cout << "[Settings] No " << path_ << " yet, using defaults" << std::endl; return; }
    auto kb = KeyboardBindings(), pad = ControllerBindings();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        const int n = atoi(v.c_str());
        if (k == "fullscreen") fullscreen = n != 0;
        else if (k == "arbitrary_resolution") arbitrary_resolution = n != 0;
        else if (k == "texture_filtering") texture_filtering = n != 0;
        else if (k == "custom_textures") custom_textures = n != 0;
        else if (k == "draw_all_geometry") draw_all_geometry = n != 0;
        else if (k == "software_renderer") software_renderer = n != 0;
        else if (k == "msaa") msaa = (n >= 8) ? 8 : (n >= 4) ? 4 : (n >= 2) ? 2 : 0;
        else if (k == "supersample") supersample = (std::max)(1, (std::min)(3, n));
        else if (k == "native_resolution") native_resolution = n != 0;
        else if (k == "hide_soft_keys") hide_soft_keys = (std::max)(0, (std::min)(2, n));
        else if (k == "fov_percent") fov_percent = (std::max)(50, (std::min)(200, n));
        else if (k == "left_stick_dpad") left_stick_dpad = n != 0;
        else if (k == "diagonal_movement") diagonal_movement = n != 0;
        else if (k == "analog_movement") analog_movement = n != 0;
        else if (k == "custom_player_model") custom_player_model = n != 0;
        else if (k == "skip_intro") skip_intro = n != 0;
        else if (k == "pause_on_focus_loss") pause_on_focus_loss = n != 0;
        else if (k == "debug_mode") { debug_mode = n != 0; debug_mode_in_ini = true; }
        else if (k == "cheat_infinite_health") cheat_infinite_health = n != 0;
        else if (k == "cheat_infinite_ammo") cheat_infinite_ammo = n != 0;
        else if (k == "dbg_show_fps") dbg_show_fps = n != 0;
        else if (k == "dbg_lua_debug_mode") dbg_lua_debug_mode = n != 0;
        else if (k == "dbg_frame_rate_test") dbg_frame_rate_test = n != 0;
        else if (k == "window_saved") window_saved = n != 0;
        else if (k == "window_x") window_x = n;
        else if (k == "window_y") window_y = n;
        else if (k == "window_w") window_w = n;
        else if (k == "window_h") window_h = n;
        else if (k == "touch_mode") touch_mode = (std::max)(0, (std::min)(2, n));
        else if (k == "touch_opacity") touch_opacity = (std::max)(10, (std::min)(100, n));
        else if (k.rfind("touch.", 0) == 0) {   // touch.<control>.x / .y / .size
            for (int c = 0; c < kTouchControls; ++c) {
                const std::string base = std::string("touch.") + TouchControls::Name(c) + ".";
                if (k == base + "x") touch_x[c] = ParseClamped(v, 0.0f, 1.0f);
                else if (k == base + "y") touch_y[c] = ParseClamped(v, 0.0f, 1.0f);
                else if (k == base + "size") touch_size[c] = ParseClamped(v, 0.05f, 0.9f);
            }
        }
        else if (k.rfind("kb.", 0) == 0) { int i = ActionIndex(k.c_str() + 3); if (i >= 0) kb[i] = ParseList(v); }
        else if (k.rfind("pad.", 0) == 0) { int i = ActionIndex(k.c_str() + 4); if (i >= 0) pad[i] = ParseList(v); }
    }
    SetKeyboardBindings(kb);
    SetControllerBindings(pad);
    std::cout << "[Settings] Loaded " << path_ << std::endl;
}

void Settings::Save() {
    auto kb = KeyboardBindings(), pad = ControllerBindings();
    std::ofstream f(Platform::U8Path(path_), std::ios::trunc);
    if (!f) { std::cerr << "[Settings] Cannot write " << path_ << std::endl; return; }
    f << "# MGS Mobile native port settings (written by the F1 overlay; edit while the game is closed)\n";
    f << "[General]\n";
    f << "fullscreen=" << fullscreen.load() << "\n";
    f << "arbitrary_resolution=" << arbitrary_resolution.load() << "\n";
    f << "texture_filtering=" << texture_filtering.load() << "\n";
    f << "custom_textures=" << custom_textures.load() << "\n";
    f << "draw_all_geometry=" << draw_all_geometry.load() << "\n";
    f << "software_renderer=" << software_renderer.load() << "  # 1 = the game's own software renderer (takes effect on restart)\n";
    f << "msaa="<< msaa.load() << "  # 0 off, 2, 4, 8\n";
    f << "supersample=" << supersample.load() << "  # 1-3\n";
    f << "native_resolution=" << native_resolution.load() << "  # 1 = render at the phone's resolution, scaled up (pixelated)\n";
    f << "hide_soft_keys="<< hide_soft_keys.load() << "  # 0 show, 1 smart (gameplay only), 2 all\n";
    f << "fov_percent=" << fov_percent.load() << "\n";
    f << "left_stick_dpad=" << left_stick_dpad.load() << "\n";
    f << "diagonal_movement=" << diagonal_movement.load() << "  # 8-way movement\n";
    f << "analog_movement=" << analog_movement.load() << "  # left stick / touch D-pad steer at any angle\n";
    f << "custom_player_model=" << custom_player_model.load() << "  # customModel/snake.mgsm in place of Snake\n";
    f << "skip_intro=" << skip_intro.load() << "  # skip the boot logos\n";
    f << "pause_on_focus_loss=" << pause_on_focus_loss.load() << "\n";
    f << "debug_mode=" << debug_mode.load() << "  # level select, inventory editor and cheats, on-screen GL warnings, verbose log\n";
    f << "cheat_infinite_health=" << cheat_infinite_health.load() << "  # debug mode only\n";
    f << "cheat_infinite_ammo=" << cheat_infinite_ammo.load() << "  # debug mode only\n";
    f << "dbg_show_fps=" << dbg_show_fps.load() << "  # original debug commands, debug mode only\n";
    f << "dbg_lua_debug_mode=" << dbg_lua_debug_mode.load() << "\n";
    f << "dbg_frame_rate_test=" << dbg_frame_rate_test.load() << "\n";
    f << "[Touch]\n";
    f << "touch_mode=" << touch_mode.load() << "  # 0 off, 1 auto (shown once the screen is touched), 2 on\n";
    f << "touch_opacity=" << touch_opacity.load() << "  # percent\n";
    for (int c = 0; c < kTouchControls; ++c) {
        const std::string base = std::string("touch.") + TouchControls::Name(c) + ".";
        f << base << "x=" << touch_x[c].load() << "\n" << base << "y=" << touch_y[c].load() << "\n" << base << "size=" << touch_size[c].load() << "\n";
    }
    f << "[Window]\n";
    f << "window_saved=" << window_saved.load() << "\nwindow_x=" << window_x.load() << "\nwindow_y=" << window_y.load()
      << "\nwindow_w=" << window_w.load() << "\nwindow_h=" << window_h.load() << "\n";
    auto list = [](const std::vector<int>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
        return s;
    };
    const auto& a = PhoneActions();
    f << "[Keyboard]  # Windows virtual-key codes\n";
    for (size_t i = 0; i < a.size(); ++i) f << "kb." << a[i].id << "=" << list(kb[i]) << "\n";
    f << "[Controller]  # SDL gamepad buttons (0 south, 1 east, 2 west, 3 north, 4 back, 6 start, 9 LB, 10 RB, 11-14 d-pad, 26 LT, 27 RT)\n";
    for (size_t i = 0; i < a.size(); ++i) f << "pad." << a[i].id << "=" << list(pad[i]) << "\n";
}

std::string PadButtonName(int b) {
#ifdef MGS_3DS   // the 3DS buttons in those positions (gamepad_3ds.cpp)
    static const char* names[] = {
        "B", "A", "Y", "X", "Select", "Guide", "Start", "Left stick click", "Right stick click", "L", "R", "D-pad Up",
        "D-pad Down", "D-pad Left", "D-pad Right", "Misc 1", "Right paddle 1", "Left paddle 1", "Right paddle 2",
        "Left paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "ZL", "ZR" };
#elif defined(MGS_VITA)   // the Vita buttons (vita/gamepad_vita.cpp; rear touch corners as L2 / R2)
    static const char* names[] = {
        "Cross", "Circle", "Square", "Triangle", "Select", "Guide", "Start", "Left stick click", "Right stick click",
        "L", "R", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Misc 1", "Right paddle 1", "Left paddle 1",
        "Right paddle 2", "Left paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6",
        "Rear touch left", "Rear touch right" };
#else
    static const char* names[] = {
        "South (A / Cross)", "East (B / Circle)", "West (X / Square)", "North (Y / Triangle)", "Back / Select",
        "Guide", "Start", "Left stick click", "Right stick click", "LB / L1", "RB / R1", "D-pad Up", "D-pad Down",
        "D-pad Left", "D-pad Right", "Misc 1", "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2",
        "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT / L2", "RT / R2" };
#endif
    return (b >= 0 && b < PAD_BUTTON_COUNT) ? names[b] : "?";
}
