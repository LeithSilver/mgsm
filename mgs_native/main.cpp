#include <SDL3/SDL.h>
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED   // desktop: our own main() (SDL_SetMainReady below); Android: SDL's Java side calls it
#endif
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <cstdio>
#include "loader.h"
#include "vfs.h"
#include "runtime.h"
#include "gles.h"
#include "audio.h"
#include "gamepad.h"
#include "settings.h"
#include "overlay.h"
#include "assetviewer.h"
#include "gametools.h"
#include "photomode.h"
#include "screenshot.h"
#include "installer.h"
#include "textures.h"
#include "custommodel.h"
#include "autotest.h"
#include "platform.h"
#include "keys.h"
#include "touchcontrols.h"
#ifdef __ANDROID__
#include "glhost_egl.h"
#include <unistd.h>
#endif

// Guest screen: 240 px on the short side, the window's aspect on the long one (arbitrary resolution; off or
// --qvga: always 240x320 / 320x240). Default window: 2x portrait, or the size/position saved last time.
// Options live in mgs_mobile.ini and the F1 overlay (F1 or the controller's Back button). Command line:
// --window WxH, --fullscreen (F11 / Alt+Enter toggle), --filter (F9 toggle), --qvga, --pillarbox, --software.
//
// Threads: this (UI) thread owns the SDL window and pumps its events; the emu thread runs the game and renders
// with GL offscreen (gles.cpp), handing finished BGRA frames to PresentBGRA; the gamepad thread polls pads.
// Other threads reach the UI thread through SDL user events (Post).
constexpr int GAME_WIDTH  = 240;
constexpr int GAME_HEIGHT = 320;
constexpr int DEFAULT_WIN_WIDTH  = GAME_WIDTH * 2;
constexpr int DEFAULT_WIN_HEIGHT = GAME_HEIGHT * 2;

// SDL user events (offsets from g_event_base), posted from any thread.
enum UiEvent {
    EV_TOGGLE_OVERLAY,   // controller Back button / overlay close button (code 2 = close)
    EV_SYNC_FULLSCREEN,  // the fullscreen setting changed in the overlay
    EV_PHOTO_MODE,       // the overlay's photo mode button
    EV_EMU_FAILED,       // the game's code stopped with an error (release build: tell the player)
    EV_FRAME,            // a new frame is in g_backbuffer
    EV_CLOSE,            // close the window (the game quit, or the test script finished)
    EV_KEY,              // test script key: code = VK, data1 = down
    EV_RESIZE,           // test script: code = width, data1 = height (client size)
    EV_COUNT
};
static Uint32 g_event_base = 0;

static void Post(UiEvent ev, int code = 0, intptr_t data = 0) {
    if (!g_event_base) return;
    SDL_Event e{};
    e.type = g_event_base + ev;
    e.user.code = code;
    e.user.data1 = reinterpret_cast<void*>(data);
    SDL_PushEvent(&e);
}

// Release build (GUI subsystem, no console; tools/make_dist.py): the log goes to mgs_mobile.log next to the exe
// and fatal errors are shown in a message box. With a console or redirected output (dev build, tests) neither.
static bool g_log_to_file = false;

static void SetupReleaseLog() {
#ifdef _WIN32
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return;
    const std::wstring path = (Platform::ExeDir() / "mgs_mobile.log").wstring();
    FILE* f = nullptr;
    if (_wfreopen_s(&f, path.c_str(), L"w", stdout) != 0) return;
    _wfreopen_s(&f, path.c_str(), L"a", stderr);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::ios::sync_with_stdio(true);
    g_log_to_file = true;
#endif
}

static void FatalBox(const char* what) {
    std::cerr << what << std::endl;
    if (!g_log_to_file) return;
    std::string msg = std::string(what) + "\n\nDetails are in mgs_mobile.log next to mgs_mobile.exe.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Metal Gear Solid Mobile", msg.c_str(), nullptr);
}

static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;
// Direct presentation (desktop default; MGS_PRESENT=readback for the old path): the game's GL context draws to
// g_window itself (GLES::PresentToWindow), so there is no 2D renderer and no frame copy through this thread.
static bool g_direct = false;
static std::atomic<int> g_pix_w{0}, g_pix_h{0};   // g_window's size in pixels (UI thread writes, emu thread reads)
static SDL_Texture* g_texture = nullptr;
static int g_tex_w = 0, g_tex_h = 0;
static std::atomic<bool> g_focused{true};
static std::mutex g_frame_mutex;
// Last presented frame, 32-bit BGRA (0xAARRGGBB words). Software frames without a GL context are the guest
// screen size (converted from RGB565); GL frames come at the output size chosen from the window
// (see S3ERuntime::ApplyDisplay).
static std::vector<uint32_t> g_backbuffer(GAME_WIDTH * GAME_HEIGHT, 0);
static int g_frame_w = GAME_WIDTH, g_frame_h = GAME_HEIGHT;
static std::atomic<bool> g_frame_pending{false};

static void PresentBGRA(const uint32_t* px, int w, int h) {   // emu thread
    if (!px) {   // direct presentation: only the frame size, for mouse and touch mapping
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_frame_w = w; g_frame_h = h;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_backbuffer.assign(px, px + static_cast<size_t>(w) * h);
        g_frame_w = w; g_frame_h = h;
    }
    if (!g_frame_pending.exchange(true)) Post(EV_FRAME);   // one redraw queued at a time
}

// Where the frame is drawn in the window (aspect-fit, centred, pixels): used for drawing and mouse mapping.
struct FramePlacement { int dx, dy, dw, dh; double scale; };
static FramePlacement PlaceFrame(int cw, int ch, int fw, int fh) {
    double s = (std::min)(double(cw) / fw, double(ch) / fh);
    int dw = static_cast<int>(fw * s + 0.5), dh = static_cast<int>(fh * s + 0.5);
    return { (cw - dw) / 2, (ch - dh) / 2, dw, dh, s };
}

static void DrawWindow() {   // UI thread
    if (!g_renderer) return;
    int cw = 0, ch = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &cw, &ch);
    FramePlacement p{};
    int fw = 0;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        if (!g_texture || g_tex_w != g_frame_w || g_tex_h != g_frame_h) {
            if (g_texture) SDL_DestroyTexture(g_texture);
            g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, g_frame_w, g_frame_h);
            g_tex_w = g_frame_w; g_tex_h = g_frame_h;
        }
        if (g_texture) SDL_UpdateTexture(g_texture, nullptr, g_backbuffer.data(), g_frame_w * 4);
        p = PlaceFrame(cw, ch, g_frame_w, g_frame_h);
        fw = g_frame_w;
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    if (g_texture) {
        // Shrinking: smooth (like GDI's HALFTONE before); same size or larger: sharp pixels
        SDL_SetTextureScaleMode(g_texture, p.dw < fw ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        const SDL_FRect dst = { float(p.dx), float(p.dy), float(p.dw), float(p.dh) };
        SDL_RenderTexture(g_renderer, g_texture, nullptr, &dst);
    }
    SDL_RenderPresent(g_renderer);
}

// Input sources -> phone keys. A phone key is down while any source maps to it: several PC keys can share
// one (Enter/Space/J = OK) and the gamepad thread reports its own set. Bindings come from Settings. The
// runtime turns transitions into pressed/released edges. Keyboard runs on the UI thread, the gamepad on its
// own: g_input_mutex.
static std::mutex g_input_mutex;
static bool g_vk_down[256];
static bool g_pad_keys[256];
static bool g_touch_keys[256];
static void RefreshPhoneKey(uint32_t k) {   // g_input_mutex held
    bool any = g_pad_keys[k] || g_touch_keys[k];
    for (int vk = 0; vk < 256 && !any; ++vk)
        if (g_vk_down[vk]) for (uint8_t k2 : Settings::Instance().KeysForVK(vk)) if (k2 == k) { any = true; break; }
    S3ERuntime::Instance().SetKeyState(k, any);
}
static void SetHostKey(int vk, bool down) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (vk <= 0 || vk >= 256 || g_vk_down[vk] == down) return;   // ignore auto-repeat
    g_vk_down[vk] = down;
    for (uint8_t k : Settings::Instance().KeysForVK(vk)) RefreshPhoneKey(k);
}
static void SetPadKeys(const bool keys[256]) {
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        for (uint32_t k = 0; k < 256; ++k) {
            any = any || keys[k];
            if (g_pad_keys[k] != keys[k]) { g_pad_keys[k] = keys[k]; RefreshPhoneKey(k); }
        }
    }
    if (any) TouchControls::Instance().OtherInput();   // (Auto: a controller in use hides the touch controls)
}
static void SetTouchKeys(const bool keys[256]) {   // UI thread
    std::lock_guard<std::mutex> lock(g_input_mutex);
    for (uint32_t k = 0; k < 256; ++k)
        if (g_touch_keys[k] != keys[k]) { g_touch_keys[k] = keys[k]; RefreshPhoneKey(k); }
}
static void ReleaseKeyboard() {   // UI thread
    for (int vk = 0; vk < 256; ++vk) if (g_vk_down[vk]) SetHostKey(vk, false);
}

// Borderless fullscreen on the window's display, and back.
static bool IsFullscreen() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN); }
static void SetFullscreen(bool on) {
    if (!g_window) return;
#ifdef __ANDROID__
    on = true;   // always: SDL only hides the system bars while the window is fullscreen
#endif
    SDL_SetWindowFullscreen(g_window, on);
    Settings::Instance().fullscreen = on;
}

// Pushes the current settings into the subsystems (any thread; RefreshView needs the emu thread).
static void ApplySettings() {
    Settings& s = Settings::Instance();
    S3ERuntime::Instance().SetFixedQVGA(!s.arbitrary_resolution);
    S3ERuntime::Instance().SetPillarbox(!s.arbitrary_resolution);
    S3ERuntime::Instance().SetFovPercent(s.fov_percent);
    S3ERuntime::Instance().SetHideSoftKeys(s.hide_soft_keys);
    GLES::Instance().SetTextureFiltering(s.texture_filtering);
    TexturePack::SetEnabled(s.custom_textures);
    // (the software renderer's frame is only scaled up: anti-aliasing would just cost GPU time)
    if (S3ERuntime::UseGL()) GLES::Instance().SetAntiAliasing(s.msaa, s.supersample);
    else GLES::Instance().SetAntiAliasing(0, 1);
    GLES::Instance().SetNativeResolution(S3ERuntime::UseGL() && s.native_resolution);
    S3ERuntime::Instance().SetDrawAllGeometry(s.draw_all_geometry);
#ifdef __ANDROID__
    S3ERuntime::Instance().SetPauseOnFocusLoss(true);   // backgrounded / notification shade: like a call on the phone
#else
    S3ERuntime::Instance().SetPauseOnFocusLoss(s.pause_on_focus_loss);
#endif
    const bool dbg = s.debug_mode;
    S3ERuntime::Instance().SetDebugCommands(dbg && s.dbg_show_fps, dbg && s.dbg_lua_debug_mode, dbg && s.dbg_frame_rate_test);
}

static void SetOverlayOpen(bool open) {   // UI thread
    if (open == Overlay::Instance().IsOpen()) return;
    if (open) { ReleaseKeyboard(); TouchControls::Instance().ReleaseAll(); }   // the game shouldn't see keys held while the menu opens
    Overlay::Instance().SetOpen(open);
#ifndef __ANDROID__   // (Android: that would pop up the on-screen keyboard; the overlay has no text fields)
    if (open) SDL_StartTextInput(g_window); else SDL_StopTextInput(g_window);
#endif
    if (!open) Settings::Instance().Save();   // final state of anything edited (saves are throttled while editing)
}

// Photo mode (debug mode, gameplay only): the game's keys are released and every key goes to the free camera.
static void SetPhotoMode(bool on) {   // UI thread
    PhotoMode& pm = PhotoMode::Instance();
    if (on == pm.Active()) return;
    if (on && (!Settings::Instance().debug_mode || !S3ERuntime::Instance().CanChangeRoom())) return;
    if (on) { ReleaseKeyboard(); SetOverlayOpen(false); }
    pm.SetActive(on);
}

// Window size (client area) and position, saved a moment after the user stops resizing/moving it (not while
// fullscreen, maximized or minimized).
static bool g_placement_dirty = false;
static std::chrono::steady_clock::time_point g_placement_changed;
static void SaveWindowPlacement() {
    static const bool testing = getenv("MGS_AUTOTEST") != nullptr;   // scripted resizes aren't the player's choice
    if (testing || !g_window || (SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) return;
    int w = 0, h = 0, x = 0, y = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_GetWindowPosition(g_window, &x, &y);
    Settings& s = Settings::Instance();
    s.window_w = w; s.window_h = h;
    s.window_x = x; s.window_y = y;
    s.window_saved = true;
    s.Save();
}

// Window coordinates (SDL points) -> frame pixels, for the overlay's mouse input.
static void MouseToFrame(float wx, float wy, float& fx, float& fy) {
    const float density = SDL_GetWindowPixelDensity(g_window);
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(g_window, &cw, &ch);
    FramePlacement p;
    { std::lock_guard<std::mutex> lock(g_frame_mutex); p = PlaceFrame(cw, ch, g_frame_w, g_frame_h); }
    fx = static_cast<float>((wx * density - p.dx) / p.scale);
    fy = static_cast<float>((wy * density - p.dy) / p.scale);
}

// A finger (or, for testing on a PC with touch controls on, the mouse) on the window -> frame pixels for the
// touch controls. Not while the overlay is open: it takes touches as mouse input.
static void HandleTouch(int64_t id, float wx, float wy, int phase) {   // UI thread; window pixels; 0 down 1 move 2 up
    TouchControls& tc = TouchControls::Instance();
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(g_window, &cw, &ch);
    FramePlacement p;
    int fw, fh;
    { std::lock_guard<std::mutex> lock(g_frame_mutex); p = PlaceFrame(cw, ch, g_frame_w, g_frame_h); fw = g_frame_w; fh = g_frame_h; }
    const float fx = static_cast<float>((wx - p.dx) / p.scale), fy = static_cast<float>((wy - p.dy) / p.scale);
    if (phase == 0) { if (!Overlay::Instance().IsOpen() && !PhotoMode::Instance().Active()) tc.FingerDown(id, fx, fy, fw, fh); }
    else if (phase == 1) tc.FingerMove(id, fx, fy, fw, fh);
    else tc.FingerUp(id, fx, fy, fw, fh);
}
static bool MouseIsTouch(SDL_MouseID which) {   // a real mouse standing in for a finger (touch controls On)
    return which != SDL_TOUCH_MOUSEID && Settings::Instance().touch_mode == 2 && !Overlay::Instance().IsOpen() && !PhotoMode::Instance().Active();
}

// One PC key, from SDL or the test script.
static void HandleKey(int vk, bool down, bool repeat, SDL_Keymod mod) {   // UI thread
    Overlay& ov = Overlay::Instance();
    if (down && !repeat) TouchControls::Instance().OtherInput();
    if (!down) {
        if (ov.IsOpen()) ov.OnKey(vk, false);
        PhotoMode::Instance().OnKey(vk, false);
        SetHostKey(vk, false);
        return;
    }
    if (vk == VK_RETURN && (mod & SDL_KMOD_ALT)) {   // Alt+Enter
        if (!repeat) { SetFullscreen(!IsFullscreen()); Settings::Instance().Save(); }
        return;
    }
    if (ov.IsOpen() && ov.CapturingKey()) { if (!repeat) ov.OnCaptureKey(vk); return; }
    if (vk == VK_F1) { if (!repeat) SetOverlayOpen(!ov.IsOpen()); return; }   // settings overlay
    if (vk == VK_F12) {   // screenshot; Shift+F12 (debug mode): dump the next GL frame
        if (!repeat) {
            if (mod & SDL_KMOD_SHIFT) { if (Settings::Instance().debug_mode) GLES::Instance().RequestDump(); }
            else GLES::Instance().RequestScreenshot();
        }
        return;
    }
    if (vk == VK_F5) { if (!repeat) SetPhotoMode(!PhotoMode::Instance().Active()); return; }
    if (vk == VK_F11) { if (!repeat) { SetFullscreen(!IsFullscreen()); Settings::Instance().Save(); } return; }
    if (vk == VK_F9) {   // texture filtering on/off
        if (!repeat) {
            Settings::Instance().texture_filtering = !Settings::Instance().texture_filtering;
            ApplySettings();
            Settings::Instance().Save();
        }
        return;
    }
    if (ov.IsOpen()) {
        if (vk == VK_ESCAPE && !repeat) { SetOverlayOpen(false); return; }
        ov.OnKey(vk, true);
        return;
    }
    if (PhotoMode::Instance().Active()) {
        if (!repeat && vk == VK_F6) S3ERuntime::Instance().StepFrame();
        else if (!repeat && vk == VK_ESCAPE) SetPhotoMode(false);
        else PhotoMode::Instance().OnKey(vk, true);
        return;
    }
    SetHostKey(vk, true);
}

// UTF-8 text from SDL -> code points for the overlay's text fields.
static void HandleText(const char* s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        unsigned cp = *p, extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        ++p;
        for (unsigned i = 0; i < extra && (*p & 0xC0) == 0x80; ++i, ++p) cp = (cp << 6) | (*p & 0x3F);
        if (cp >= 32) Overlay::Instance().OnChar(cp);
    }
}

static void RequestClose(bool& running) {   // UI thread
    if (g_window) SDL_HideWindow(g_window);   // feedback at once; shutdown continues after the event loop
    S3ERuntime::Instance().RequestQuit();
    running = false;
}

static void HandleEvent(const SDL_Event& e, bool& running) {   // UI thread
    Overlay& ov = Overlay::Instance();
    PhotoMode& pm = PhotoMode::Instance();
    // Window events of the hidden GL host window (16x16) are not ours
    if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST && e.window.windowID != SDL_GetWindowID(g_window)) return;
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            RequestClose(running);
            return;
        case SDL_EVENT_WINDOW_EXPOSED:
            DrawWindow();
            return;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (e.window.data1 > 0 && e.window.data2 > 0) {
                S3ERuntime::Instance().SetHostClientSize(e.window.data1, e.window.data2);
                g_pix_w = e.window.data1; g_pix_h = e.window.data2;
            }
            DrawWindow();
            [[fallthrough]];
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_MOVED:
            g_placement_dirty = true;
            g_placement_changed = std::chrono::steady_clock::now();
            return;
        case SDL_EVENT_WINDOW_FOCUS_LOST:   // keys released while unfocused never arrive: release everything
            g_focused = false;
            ReleaseKeyboard();
            TouchControls::Instance().ReleaseAll();
            pm.ReleaseAll();
            S3ERuntime::Instance().SetFocusLost(true);
            return;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            g_focused = true;
            S3ERuntime::Instance().SetFocusLost(false);
            return;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (e.key.scancode == SDL_SCANCODE_AC_BACK) {   // Android Back: the overlay, like the controller's Back
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) SetOverlayOpen(!ov.IsOpen());
                return;
            }
            if (const int vk = VKFromScancode(e.key.scancode)) HandleKey(vk, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat, e.key.mod);
            return;
        case SDL_EVENT_TEXT_INPUT:
            if (ov.IsOpen()) HandleText(e.text.text);
            return;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            int cw = 0, ch = 0;
            SDL_GetWindowSizeInPixels(g_window, &cw, &ch);
            const int phase = e.type == SDL_EVENT_FINGER_DOWN ? 0 : e.type == SDL_EVENT_FINGER_MOTION ? 1 : 2;
            HandleTouch(static_cast<int64_t>(e.tfinger.fingerID), e.tfinger.x * cw, e.tfinger.y * ch, phase);
            return;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (MouseIsTouch(e.motion.which) && (e.motion.state & SDL_BUTTON_LMASK)) {
                const float density = SDL_GetWindowPixelDensity(g_window);
                HandleTouch(-1, e.motion.x * density, e.motion.y * density, 1);
            }
            if (ov.IsOpen()) {
                float x, y; MouseToFrame(e.motion.x, e.motion.y, x, y);
                ov.OnMouseMove(x, y);
            } else if (pm.Active() && (e.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK))) {
                const float density = SDL_GetWindowPixelDensity(g_window);   // look around while a button is held
                pm.OnMouseLook(static_cast<int>(e.motion.xrel * density), static_cast<int>(e.motion.yrel * density));
            }
            return;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (e.button.button == SDL_BUTTON_LEFT && (MouseIsTouch(e.button.which) || !down)) {
                const float density = SDL_GetWindowPixelDensity(g_window);
                HandleTouch(-1, e.button.x * density, e.button.y * density, down ? 0 : 2);
            }
            if (ov.IsOpen() || pm.Active()) SDL_CaptureMouse(down);   // keep drags going outside the window
            if (!ov.IsOpen()) return;
            float x, y; MouseToFrame(e.button.x, e.button.y, x, y);
            ov.OnMouseMove(x, y);
            if (e.button.button == SDL_BUTTON_LEFT) ov.OnMouseButton(0, down);
            else if (e.button.button == SDL_BUTTON_RIGHT) ov.OnMouseButton(1, down);
            else if (e.button.button == SDL_BUTTON_MIDDLE) ov.OnMouseButton(2, down);
            return;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            const float notches = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            if (ov.IsOpen()) ov.OnMouseWheel(notches);
            else if (pm.Active()) pm.OnMouseWheel(notches);
            return;
        }
        default: break;
    }
    if (e.type < g_event_base || e.type >= g_event_base + EV_COUNT) return;
    switch (static_cast<UiEvent>(e.type - g_event_base)) {
        case EV_TOGGLE_OVERLAY:
            SetOverlayOpen(e.user.code == 2 ? false : !ov.IsOpen());   // 2 = close
            break;
        case EV_SYNC_FULLSCREEN:
            if (Settings::Instance().fullscreen != IsFullscreen()) SetFullscreen(Settings::Instance().fullscreen);
            break;
        case EV_PHOTO_MODE:
            SetPhotoMode(!pm.Active());
            if (!pm.Active()) SetOverlayOpen(false);   // leaving from the menu closes it too
            break;
        case EV_EMU_FAILED:
            if (g_log_to_file)
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Metal Gear Solid Mobile",
                    "The game stopped because of an internal error.\n\nDetails are in mgs_mobile.log next to "
                    "mgs_mobile.exe. Please include that file when you report the problem.", g_window);
            RequestClose(running);
            break;
        case EV_FRAME:
            g_frame_pending = false;
            DrawWindow();
            break;
        case EV_CLOSE:
            RequestClose(running);
            break;
        case EV_KEY:
            {   // VK + 0x100: with Shift held (e.g. 0x17B = Shift+F12, a GL frame dump)
                const SDL_Keymod mod = (e.user.code & 0x100) ? static_cast<SDL_Keymod>(SDL_GetModState() | SDL_KMOD_LSHIFT) : SDL_GetModState();
                HandleKey(e.user.code & 0xFF, e.user.data1 != nullptr, false, mod);
            }
            break;
        case EV_RESIZE:
            SDL_SetWindowSize(g_window, e.user.code, static_cast<int>(reinterpret_cast<intptr_t>(e.user.data1)));
            break;
        default: break;
    }
}

void EmuThreadFunc(Cpu* uc, uint32_t entry_point) {
    std::cout << "[EmuThread] Starting execution at 0x" << std::hex << entry_point << std::dec << "..." << std::endl;
    int err = uc->Start(entry_point, 0xDEADBEEF);
    if (err != Cpu::OK && !S3ERuntime::Instance().IsQuitRequested()) {
        uint32_t pc = 0, lr = 0;
        uc->RegRead(Cpu::REG_PC, &pc);
        uc->RegRead(Cpu::REG_LR, &lr);
        std::cerr << "[EmuThread] Emulation stopped: " << uc->ErrorText(err)
                  << " at PC=0x" << std::hex << pc << ", LR=0x" << lr << std::dec << std::endl;
        static const int regs[] = { Cpu::REG_R0, Cpu::REG_R1, Cpu::REG_R2, Cpu::REG_R3, Cpu::REG_R4,
            Cpu::REG_R5, Cpu::REG_R6, Cpu::REG_R7, Cpu::REG_R8, Cpu::REG_R9, Cpu::REG_R10,
            Cpu::REG_R11, Cpu::REG_R12, Cpu::REG_SP };
        std::cerr << "[EmuThread] Registers:" << std::hex;
        for (int i = 0; i < 14; ++i) { uint32_t v = 0; uc->RegRead(regs[i], &v); std::cerr << (i == 13 ? " sp=" : " r") << (i == 13 ? "" : std::to_string(i) + "=") << v; }
        std::cerr << std::dec << std::endl;
        Post(EV_EMU_FAILED);
    } else {
        std::cout << "[EmuThread] Emulation stopped cleanly." << std::endl;
    }
}

int main(int argc, char** argv) {
    SetupReleaseLog();
    Platform::StartLog();
    std::cout << "========================================================" << std::endl;
    std::cout << " Metal Gear Solid Mobile - Native PC Port (N-Gage 2.0)" << std::endl;
    std::cout << " Standalone ARM CPU Recompiler & Execution Shim" << std::endl;
    std::cout << "========================================================" << std::endl;

    Settings& settings = Settings::Instance();
    settings.Load();
#ifdef __ANDROID__
    settings.fullscreen = true;
#endif
    namespace fs = std::filesystem;

    // Game files next to the exe (not the working directory, so shortcuts work): the distribution layout
    // (game/mgs_ngi, game/data) or the repo layout; the plain relative paths as a last resort.
    // No game files yet: first-run setup installs them into game/ from the player's Metal.Gear.Solid.n-gage
    // (installer.cpp; "--install <package>" does it without the window). Paths are UTF-8.
    bool dist_layout = false, need_setup = false;
    std::string bin_path = "extracted_mgs/mgs_ngi";
    std::string asset_dir = "EKA2L1/data/drives/c/Private/20010925/all";
    const std::string exe_dir = Platform::U8String(Platform::ExeDir());   // ends with a separator
    {
        std::error_code ec;
        if (argc >= 3 && std::string(argv[1]) == "--install") {
            std::string err;
            const bool ok = Installer::Install(fs::path(argv[2]), exe_dir, nullptr, &err);
            if (!ok) std::cerr << "[Setup] " << err << std::endl;
            return ok ? 0 : 1;
        }
        const bool repo = fs::exists(Platform::U8Path(exe_dir + "extracted_mgs/mgs_ngi"), ec);
        if (!Installer::Installed(exe_dir) && !repo && argc < 2) {
#if defined(_WIN32)
            SetProcessDPIAware();
            if (!Installer::RunSetupWindow(exe_dir)) return 0;
#elif defined(__ANDROID__)
            need_setup = true;   // the setup screen, once the window exists
#else
            FatalBox("No game files yet. Install them with: mgs_mobile --install <Metal.Gear.Solid.n-gage>");
            return 1;
#endif
        }
        if (repo && !Installer::Installed(exe_dir)) { bin_path = exe_dir + "extracted_mgs/mgs_ngi"; asset_dir = exe_dir + asset_dir; }
    }
    auto use_install = [&] {   // the installed copy in game/
        if (!Installer::Installed(exe_dir)) return;
        bin_path = exe_dir + "game/mgs_ngi"; asset_dir = exe_dir + "game/data"; dist_layout = true;
    };
    use_install();
    int win_w = DEFAULT_WIN_WIDTH, win_h = DEFAULT_WIN_HEIGHT;
    bool window_arg = false;

    // Usage: mgs_mobile [binary] [asset_dir] [--window WxH] [--fullscreen] [--filter] [--qvga] [--pillarbox] [--software]
    // (command-line options override mgs_mobile.ini for this run and are then saved like any other change)
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window" && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2 || win_w < 64 || win_h < 64) {
                std::cerr << "Bad --window value, expected WxH" << std::endl;
                win_w = DEFAULT_WIN_WIDTH; win_h = DEFAULT_WIN_HEIGHT;
            } else {
                window_arg = true;
            }
        } else if (a == "--fullscreen") {
            settings.fullscreen = true;
        } else if (a == "--pillarbox" || a == "--qvga") {
            settings.arbitrary_resolution = false;
        } else if (a == "--filter") {
            settings.texture_filtering = true;
        } else if (a == "--software") {
            settings.software_renderer = true;
        } else {
            positional.push_back(Platform::U8String(fs::path(a)));   // (Windows: argv is the ANSI code page)
        }
    }
    if (positional.size() >= 1) bin_path = positional[0];
    if (positional.size() >= 2) asset_dir = positional[1];
    const bool use_saved_window = !window_arg && settings.window_saved && settings.window_w >= 64 && settings.window_h >= 64;
    if (use_saved_window) { win_w = settings.window_w; win_h = settings.window_h; }

#ifdef _WIN32
    // Render at real pixels on high-DPI screens instead of being bitmap-stretched by Windows.
    SetProcessDPIAware();
    // The window icons come from the exe's icon resource 1 (mgs_mobile.rc).
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // the gamepad thread gates on our focus instead
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   // Back opens the overlay instead of closing the app
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    SDL_SetMainReady();   // our own main(), not SDL_main
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            FatalBox((std::string("Could not start SDL: ") + SDL_GetError()).c_str());
            return 1;
        }
        std::cerr << "[Main] SDL gamepad init failed (" << SDL_GetError() << "), controllers off" << std::endl;
    }
    // Audio is opened later from the emu thread (AudioOut::Start, on the game's first sound call), but the
    // subsystem is brought up here: initialising it there raced with this thread's event pump, which reads the
    // audio subsystem's lock (SDL_UpdateAudio; caught by an SDL assertion on Android).
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) std::cerr << "[Main] SDL audio init failed (" << SDL_GetError() << ")" << std::endl;
    g_event_base = SDL_RegisterEvents(EV_COUNT);
#ifdef __ANDROID__
    // Backgrounded: the game is paused (focus loss); stop redrawing it too. These events come on Android's UI
    // thread while ours may be blocked, so they're caught as they are queued.
    SDL_AddEventWatch([](void*, SDL_Event* e) {
        if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) GLES::Instance().SetBackground(true);
        else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) GLES::Instance().SetBackground(false);
        return true;
    }, nullptr);
#endif

    // The game window: a plain SDL window with a 2D renderer that shows the finished frames (shown once the game
    // is ready to start). The guest screen orientation is chosen from its size before the game starts (and
    // follows later resizes). Android: always the whole screen.
    S3ERuntime::Instance().SetHostClientSize(win_w, win_h);
#ifdef __ANDROID__
    const SDL_WindowFlags win_flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    const char* present_mode = getenv("MGS_PRESENT");
    g_direct = !(present_mode && std::string(present_mode) == "readback");
    // The GL context gles.cpp renders with: desktop GL, compatibility profile. Set before the window is made
    // (direct presentation: the window's own pixel format; readback: a hidden helper window's).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                      (g_direct ? SDL_WINDOW_OPENGL : 0);
#endif
    g_window = SDL_CreateWindow("Metal Gear Solid Mobile", win_w, win_h, win_flags);
    if (!g_window) {
        FatalBox((std::string("Could not create the game window: ") + SDL_GetError()).c_str());
        SDL_Quit();
        return 1;
    }
    if (use_saved_window) {   // only if the saved spot is still on a display
        const SDL_Point tl = { settings.window_x, settings.window_y };
        if (SDL_GetDisplayForPoint(&tl)) SDL_SetWindowPosition(g_window, settings.window_x, settings.window_y);
    }
    if (!g_direct) {
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
        if (!g_renderer) {
            FatalBox((std::string("Could not create the window renderer: ") + SDL_GetError()).c_str());
            SDL_Quit();
            return 1;
        }
        SDL_SetRenderVSync(g_renderer, 0);   // GLES::Present paces the game; presenting here must not block
    }

#ifdef __ANDROID__
    if (need_setup) {
        if (!Installer::RunSetupScreen(exe_dir, g_window, g_renderer)) { SDL_Quit(); _exit(0); }
        use_install();
    }
#endif
    // Debug mode (level select, warnings on screen, verbose log): on for the repo build, off in a distribution.
    if (!settings.debug_mode_in_ini) settings.debug_mode = !dist_layout;

    VirtualFileSystem::Instance().SetAssetRoot(asset_dir);
    if (getenv("MGS_PRELOAD")) VirtualFileSystem::Instance().PreloadAll();   // debug: the 3DS's RAM file cache (README.md section 7)

    // Textures: extracted once to textures/ next to the exe (setup does it for a new install; again here if the
    // folder was deleted), then scanned for custom replacements.
    {
        const fs::path tex_dir = Platform::ExeDir() / "textures";
        if (!TexturePack::Extracted(tex_dir)) {
            std::string err;
            if (TexturePack::Extract(asset_dir, tex_dir, nullptr, &err) < 0) std::cout << "[Textures] Extraction failed: " << err << std::endl;
        }
        TexturePack::Init(tex_dir);
    }
    CustomModel::Load(Platform::ExeDir() / "customModel" / "snake.mgsm");   // F1 > Display > Custom player model

    S3EBinary binary;
    if (!LoadS3E(bin_path, binary)) {
        FatalBox("Could not load the game program (game/mgs_ngi). Delete the game folder and run setup again.");
        SDL_Quit();
        return 1;
    }

    std::string cpu_error;
    // MGS_CPU=native: the game's code on the host CPU (32-bit ARM hosts, README.md §6.26); MGS_CPU=dynarmic: the
    // Dynarmic recompiler (§6.27); Unicorn otherwise.
    const std::string cpu_kind = getenv("MGS_CPU") ? getenv("MGS_CPU") : "unicorn";
    std::unique_ptr<Cpu> cpu = cpu_kind == "native" ? Cpu::CreateNative(&cpu_error)
                             : cpu_kind == "dynarmic" ? Cpu::CreateDynarmic(&cpu_error) : Cpu::CreateUnicorn(&cpu_error);
    std::cout << "[Main] CPU: " << cpu_kind << std::endl;
    if (!cpu) {
        FatalBox(("Could not start the CPU emulator: " + cpu_error).c_str());
        SDL_Quit();
        return 1;
    }
    Cpu* uc = cpu.get();

    // MGS_IMAGE_BASE=<hex>: load the game image elsewhere (relocated; README.md §6.25). Must not overlap the
    // trampolines, stack, framebuffer or heap (0x49FF0000, 0x4C000000, 0x4D000000, 0x50000000).
    if (const char* b = getenv("MGS_IMAGE_BASE")) SetImageBase(static_cast<uint32_t>(strtoul(b, nullptr, 16)) & ~0xFFFu);
    if (!SetupSandbox(uc, binary, ImageBase())) {
        FatalBox("Could not set up the CPU emulator's memory.");
        SDL_Quit();
        return 1;
    }

    S3ERuntime::Instance().Init(uc, binary.imports);
    if (!getenv("MGS_NO_ICF")) S3ERuntime::Instance().SetIcf(binary.config);   // MGS_NO_ICF=1: debug, ignore it
    ApplySettings();

    // Software (s3eSurface) frames: through the GL output path (overlay, pacing, screenshots); without a GL
    // context, RGB565 -> BGRA straight to the window
    S3ERuntime::Instance().SetFrameCallback([](const uint16_t* pixels, int width, int height) {
        if (GLES::Instance().PresentSoftware(pixels, width, height)) return;
        std::vector<uint32_t> bgra(static_cast<size_t>(width) * height);
        for (size_t i = 0; i < bgra.size(); ++i) {
            uint16_t p = pixels[i];
            uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            bgra[i] = ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
        }
        PresentBGRA(bgra.data(), width, height);
    });
    // OpenGL ES frames (eglSwapBuffers), with the F1 overlay drawn on top
    GLES::Instance().SetFrameCallback(PresentBGRA);
    GLES::Instance().SetOverlayCallback([](int w, int h) { Overlay::Instance().Render(w, h); });
    S3ERuntime::Instance().SetFrameTick([](bool gameplay) { GameTools::Instance().Tick(gameplay); });
    GLES::SetWarningCallback([](const std::string& w) { Overlay::Instance().AddWarning("[GLES] " + w); });
    S3ERuntime::Instance().SetDebugMessageCallback([](const std::string& m) { Overlay::Instance().AddWarning(m); });
    // The overlay or focus loss freezes the game; its clock skips the paused time. Inventory edits queued from
    // the overlay run in the game's Lua, so the overlay lets single frames through while any are pending (and
    // one for a frame capture in F1 > Models).
    GLES::Instance().SetPauseHooks(
        [] {
            S3ERuntime& rt = S3ERuntime::Instance();
            const bool overlay = Overlay::Instance().IsOpen() && !rt.LuaPending() && !AssetViewer::CaptureWaiting();
            return (overlay || rt.FocusPaused()) && !rt.IsQuitRequested();
        },
        [](long long ms) { S3ERuntime::Instance().AddPausedTime(ms); });
    Overlay::Instance().SetApplyCallback([] {   // emu thread
        ApplySettings();
        S3ERuntime::Instance().RefreshView();
        Post(EV_SYNC_FULLSCREEN);
        static auto last_save = std::chrono::steady_clock::time_point{};   // slider drags: at most 2 saves/s
        const auto now = std::chrono::steady_clock::now();
        if (now - last_save > std::chrono::milliseconds(500)) { Settings::Instance().Save(); last_save = now; }
    });
    Overlay::Instance().SetCloseCallback([] { Post(EV_TOGGLE_OVERLAY, 2); });
    TouchControls::Instance().SetKeyCallback(SetTouchKeys);
    TouchControls::Instance().SetMenuCallback([] { SetOverlayOpen(true); });   // (UI thread, from a finger event)
    Overlay::Instance().SetPhotoModeCallback([] { Post(EV_PHOTO_MODE); });
    GLES::Instance().SetScreenshotCallback([](std::vector<uint32_t> px, int w, int h) {   // emu thread; saved on a worker
        Screenshot::SaveAsync(std::move(px), w, h, [](const std::string& path, const std::string& err) {
            if (err.empty()) { std::cout << "[Screenshot] " << path << std::endl; Overlay::Instance().AddNotice("Screenshot saved: " + path); }
            else { std::cout << "[Screenshot] Failed: " << err << std::endl; Overlay::Instance().AddNotice("Screenshot failed: " + err); }
        });
    });
    Overlay::Instance().SetLevelSelect([] { return S3ERuntime::Instance().CanChangeRoom(); },
                                       [](const std::string& room, const std::string& via) { S3ERuntime::Instance().RequestRoomChange(room, via); });
    if (const char* lvl = getenv("MGS_LEVEL")) {   // debug: "room[,via]", once in gameplay
        std::string s = lvl; size_t c = s.find(',');
        S3ERuntime::Instance().RequestRoomChange(s.substr(0, c), c == std::string::npos ? "" : s.substr(c + 1));
    }
    // The game quitting by itself (Main Menu > Quit Game) closes the window like the X button.
    S3ERuntime::Instance().SetExitCallback([] { Post(EV_CLOSE); });

#ifdef __ANDROID__
    // The GL context gles.cpp renders with: OpenGL ES 1.1 on an EGL pbuffer, created on the emu thread.
    GLES::SetHost(EglPbufferHost());
    SDL_Window* gl_window = nullptr;
    SDL_GLContext gl_context = nullptr;
#else
    // The GL context gles.cpp renders with (into FBOs). Direct presentation: on the game window, which the emu
    // thread then blits each frame to and swaps. Readback: on a hidden window, frames copied out. Created here
    // (the UI thread owns windows), then made current on the emu thread by GLES on first use.
    SDL_Window* gl_window = g_direct ? nullptr : SDL_CreateWindow("MGSMobileGLHost", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_Window* ctx_window = g_direct ? g_window : gl_window;
    SDL_GLContext gl_context = ctx_window ? SDL_GL_CreateContext(ctx_window) : nullptr;
    if (gl_context) SDL_GL_MakeCurrent(ctx_window, nullptr);   // released here, bound on the emu thread
    else std::cerr << "[Main] Could not create the GL context: " << SDL_GetError() << std::endl;
    GLES::Host host{
        [ctx_window, gl_context] {
            if (!gl_context || !SDL_GL_MakeCurrent(ctx_window, gl_context)) return false;
            if (g_direct) SDL_GL_SetSwapInterval(0);   // GLES::Present paces the game
            return true;
        },
        [](const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); } };
    if (g_direct) {
        host.window_size = [](int& w, int& h) { w = g_pix_w; h = g_pix_h; };
        host.swap = [] { SDL_GL_SwapWindow(g_window); };
        GLES::Instance().SetCpuFrames(getenv("MGS_AUTOTEST") != nullptr);   // the test script's screenshots
    }
    std::cout << "[Main] Presentation: " << (g_direct ? "direct (GL to the window)" : "read back to a 2D renderer") << std::endl;
    GLES::SetHost(std::move(host));
#endif

    SDL_ShowWindow(g_window);
    if (settings.fullscreen) SetFullscreen(true);
    SDL_SyncWindow(g_window);
    {   // the real size (fullscreen, or clamped by the screen) decides the initial orientation
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        if (pw > 0 && ph > 0) { S3ERuntime::Instance().SetHostClientSize(pw, ph); g_pix_w = pw; g_pix_h = ph; }
    }
    DrawWindow();

    Gamepad::Instance().Start(
        SetPadKeys,
        [] { return g_focused.load(); },
        [] { return Overlay::Instance().IsOpen() || PhotoMode::Instance().Active(); },   // raw input to the overlay / photo mode
        [](const Gamepad::Raw& raw) { if (Overlay::Instance().IsOpen()) Overlay::Instance().OnPad(raw); else PhotoMode::Instance().OnPad(raw); },
        [] { Post(EV_TOGGLE_OVERLAY); });   // Back toggles the overlay

    // Launch emulation on worker thread
    std::thread emu_thread(EmuThreadFunc, uc, G(binary.entry_point));

    // MGS_AUTOTEST (test_splash_progression.py): scripted keys and screenshots, then close
    AutoTest::Start({
        [](int vk, bool down) { Post(EV_KEY, vk, down ? 1 : 0); },
        [](std::vector<uint32_t>& px, int& w, int& h) {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            px = g_backbuffer; w = g_frame_w; h = g_frame_h;
            return true;
        },
        [](int w, int h) { Post(EV_RESIZE, w, h); },
        [] { Post(EV_CLOSE); } });

    // Event loop
    bool running = true;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            HandleEvent(e, running);
            while (running && SDL_PollEvent(&e)) HandleEvent(e, running);
        }
        if (g_placement_dirty && std::chrono::steady_clock::now() - g_placement_changed > std::chrono::milliseconds(500)) {
            g_placement_dirty = false;
            SaveWindowPlacement();
        }
    }

    // Wait for worker thread to exit
    AutoTest::Stop();
    S3ERuntime::Instance().RequestQuit();
    if (emu_thread.joinable()) {
        emu_thread.join();
    }

    AudioOut::Instance().Stop();
    Gamepad::Instance().Stop();
    settings.Save();
    S3ERuntime::Instance().DumpExitDiagnostics();
    cpu.reset();
    if (gl_context) SDL_GL_DestroyContext(gl_context);
    if (gl_window) SDL_DestroyWindow(gl_window);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    std::cout << "[Main] Exited successfully." << std::endl;
#ifdef __ANDROID__
    // The game keeps its state in singletons: end the process so the next launch starts from scratch (SDL would
    // otherwise run main() again in the same process).
    _exit(0);
#endif
    return 0;
}
