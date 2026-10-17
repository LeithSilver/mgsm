#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "cpu.h"

// OpenGL ES 1.1 + EGL imports, serviced on a desktop GL compatibility context.
// The guest's "window surface" is an offscreen FBO: the guest sees a surf_w x surf_h screen (240x320 portrait
// or 320x240 landscape) and everything is rendered at fbo_w x fbo_h (the host window's resolution).
// eglSwapBuffers reads it back and hands BGRA pixels to the frame callback. All calls run on the emu thread.
class GLES {
public:
    static GLES& Instance();

    // The desktop GL (compatibility profile) context, created by the platform layer: make_current() binds it
    // to the calling (emu) thread, once, before the first GL call; get_proc() looks up GL functions.
    struct Host {
        std::function<bool()> make_current;
        std::function<void*(const char*)> get_proc;
        // Direct presentation (optional): the context belongs to the game window. window_size() gives its
        // drawable size and swap() shows the frame; Present then blits the finished frame (overlay included) to
        // the window's framebuffer instead of reading it back for the frame callback.
        std::function<void(int& w, int& h)> window_size;
        std::function<void()> swap;
    };
    // Frames also go to the frame callback with direct presentation (the test script's screenshots need them).
    void SetCpuFrames(bool on) { cpu_frames_ = on; }
    static void SetHost(Host host);
    static void* Proc(const char* name);   // GL function by name (current context), or null
    void SetFrameCallback(std::function<void(const uint32_t* bgra, int w, int h)> cb) { frame_cb_ = std::move(cb); }
    // Drawn into the finished frame right before it is read back (the F1 overlay). Emu thread.
    void SetOverlayCallback(std::function<void(int w, int h)> cb) { overlay_cb_ = std::move(cb); }
    // While paused() is true, Present keeps redrawing the overlay over the frozen frame and doesn't return
    // to the game; resumed(ms) then reports how long the game was frozen.
    void SetPauseHooks(std::function<bool()> paused, std::function<void(long long ms)> resumed) {
        paused_ = std::move(paused); resumed_ = std::move(resumed);
    }
    // App in the background (Android; any thread): a paused game stops redrawing until it is back.
    void SetBackground(bool on) { background_ = on; }
    // Called (emu thread) with each new [GLES] warning: unsupported formats, unhandled pnames, ...
    static void SetWarningCallback(std::function<void(const std::string&)> cb);
    void SetGuestAlloc(std::function<uint32_t(uint32_t)> alloc) { alloc_ = std::move(alloc); }

    // Screenshot of the next game frame at the render resolution (supersampled size when supersampling is on),
    // without the overlay: cb(top-down BGRA, w, h) on the emu thread. Any thread.
    void RequestScreenshot() { shot_requested_ = true; }
    // Photo mode: skip the game's 2D draws (HUD, soft keys, fades): any draw without a perspective projection.
    void SetHide2D(bool hide);
    // The CIwTexture being uploaded (its hash), for custom textures; set by runtime.cpp's upload hook.
    void SetUploadTexture(uint32_t hash);
    // The game's GL state as its last frame left it, saved when the overlay pause begins (emu thread). IwGx only
    // sends GL what changed, so the asset viewer's copy of the game starts its GL recorder from this.
    struct GuestState {
        bool valid = false;
        uint32_t matrix_mode = 0x1700;
        float mv[16] = {}, proj[16] = {}, tex[16] = {};   // texture matrix of unit 0
        bool tex2d[2] = {}, blend = false, alpha_test = false;
        uint32_t bound[2] = {};                            // GL texture per unit (the guest's names are the host's)
        int active_unit = 0, client_unit = 0;
        struct Array { bool on = false; int size = 4; uint32_t type = 0x1406, stride = 0, ptr = 0; } vertex, color, texcoord[2];
        float color4[4] = {1, 1, 1, 1};
        int unpack_align = 4;
    };
    const GuestState& PausedState() const { return paused_state_; }
    void SetScreenshotCallback(std::function<void(std::vector<uint32_t>, int, int)> cb) { shot_cb_ = std::move(cb); }

    // Debug: log every GL call of the next frame (Shift+F12 in main.cpp) as [GLDump].
    void RequestDump() { dump_requested_ = true; }
    bool IsDumping() const { return dumping_; }

    // Software renderer frame (s3eSurfaceShow, RGB565 at the guest screen size): scaled into the render target
    // and presented like a GL frame (pacing, overlay, pause, screenshots). Emu thread. False without a context.
    bool PresentSoftware(const uint16_t* rgb565, int w, int h);

    // Returns true if `name` is a gl*/egl* import (ret set in `ret`).
    bool Handle(const std::string& name, Cpu* uc, const uint32_t r[4], uint32_t& ret);

    // Texture filtering on/off (any thread; applied on the emu thread at the next GL call). Off forces
    // nearest-neighbour sampling like the phone, regardless of what the game asks for.
    void SetTextureFiltering(bool on) { filter_wanted_ = on; }
    // Anti-aliasing (any thread; applied between frames): MSAA samples (0/2/4/8, capped by the driver) and
    // supersampling factor (1..3: render at N x the output size, downsampled linearly).
    void SetAntiAliasing(int msaa, int supersample) { aa_msaa_ = msaa; aa_ss_ = supersample; }
    // Native resolution (any thread; applied between frames): render at the guest screen size (e.g. 426x240)
    // and scale up with nearest-neighbour, like the phone / the software renderer. Supersampling is ignored.
    void SetNativeResolution(bool on) { native_res_ = on; }
    bool TextureFiltering() const { return filter_wanted_; }

    // Guest screen size, the game's size in the output (same aspect) and the output (window) size: the game is
    // centred in the output with black bars, and the overlay covers all of it. Emu thread only; takes effect at once.
    void SetDisplay(int surf_w, int surf_h, int game_w, int game_h, int out_w, int out_h);

private:
    GLES() = default;
    bool EnsureContext();
    bool CreateFramebuffer();
    void ResolveFrame();
    void BindRenderTarget();
    void PresentToWindow(int w, int h);
    void Present();
    void CaptureScreenshot();
    void ApplyFilterMode();
    int EffectiveFilter(int requested) const;
#ifdef MGS_VITA
    void ApplyBaseMinFilter();
#endif
    uint32_t GuestString(const std::string& s);

    volatile bool dump_requested_ = false;
    std::atomic<bool> cpu_frames_{false};
    std::atomic<bool> shot_requested_{false};
    std::function<void(std::vector<uint32_t>, int, int)> shot_cb_;
    volatile bool filter_wanted_ = false;   // off by default, like the phone
    std::atomic<int> aa_msaa_{0}, aa_ss_{1};
    std::atomic<bool> native_res_{false};
    std::atomic<bool> background_{false};
    int aa_applied_msaa_ = 0, aa_applied_ss_ = 1;
    bool applied_native_ = false;
    bool filter_applied_ = false;
    // Filters the game requested per texture (GL name -> {min, mag}); re-applied on each toggle.
    std::unordered_map<uint32_t, std::pair<int, int>> tex_filters_;
    bool dumping_ = false;
    std::function<void(const uint32_t*, int, int)> frame_cb_;
    std::function<void(int, int)> overlay_cb_;
    std::function<bool()> paused_;
    std::function<void(long long)> resumed_;
    std::function<uint32_t(uint32_t)> alloc_;
    GuestState paused_state_;
    void SavePausedState();
};
