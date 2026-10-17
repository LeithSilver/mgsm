#pragma once

#include "loader.h"
#include "heap.h"
#include "luachunk.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class S3ERuntime {
public:
    static S3ERuntime& Instance();

    void Init(Cpu* uc, const std::vector<std::string>& imports);
    // The live guest and its import names (the asset viewer's sandbox copies the guest; emu thread only).
    Cpu* GuestCpu() const { return uc_; }
    const std::vector<std::string>& Imports() const { return imports_; }
    // The binary's embedded app.icf: the fallback for s3eConfigGetInt/String (see LoadIcf in runtime.cpp).
    void SetIcf(const std::string& text);
    void SetFrameCallback(std::function<void(const uint16_t* pixels, int width, int height)> cb);
    void SetKeyState(uint32_t key_code, bool down);   // any thread; physical transitions only (repeats ignored)
    void SwallowHeldKeys();   // held keys report nothing (not even their release) until let go
    void RequestQuit();
    // Called (emu thread) when the game itself exits via s3eDeviceExit; main.cpp closes the window.
    void SetExitCallback(std::function<void()> cb) { exit_callback_ = std::move(cb); }
    void DumpExitDiagnostics(bool force = false);   // MGS_EXIT_DIAG=1: [Hist]/[Heap]/[Recent] summary; after the emu thread stopped
    bool IsQuitRequested() const { return quit_requested_; }
    uint32_t GuestAlloc(uint32_t size);
    // Host window client size (UI thread). The guest screen is landscape when the window is wider than tall,
    // 240 px on its short side and the window's aspect on the long one (see ApplyDisplay); the game is told
    // about changes through its screen-change callbacks.
    void SetHostClientSize(int w, int h) { host_w_ = w; host_h_ = h; }
    // OpenGL ES renderer unless the software renderer setting or MGS_SOFTWARE_GX is on (IwGx's own software
    // rasteriser into the s3eSurface framebuffer). Read once, at the first call.
    static bool UseGL();
    // --qvga: always report the phone's 240x320 / 320x240 screen instead of following the window's aspect.
    void SetFixedQVGA(bool on) { fixed_qvga_ = on; }
    // --pillarbox: keep the game's 1.25 aspect cap for the 3D view (centred box on wide screens).
    void SetPillarbox(bool on) { pillarbox_ = on; }
    void SetFovPercent(int pct) { fov_percent_ = pct; }   // 100 = the game's camera
    void SetDrawAllGeometry(bool on) { draw_all_geometry_ = on; }   // ignore the camera PVS (see pvs_hook)
    // Level select (any thread): change to this room (e.g. "02_Mess_Hall") through the game's own Lua
    // RoomChange, at the next frame end during gameplay. CanChangeRoom: gameplay is the top state.
    void RequestRoomChange(const std::string& room, const std::string& via = "");   // via: a neighbour to "arrive from"
    bool CanChangeRoom() { return in_gameplay_; }
    uint32_t GuestHeapPeak() const { return heap_.PeakInUse(); }   // the guest heap's high-water mark (bytes)
    // Host Lua (luachunk.h) on the game's main Lua state, run at the next frame boundary while gameplay is the
    // top state (any thread). done(results) runs on the emu thread with chunk.Results() values (nil-padded;
    // empty if the chunk failed). Requests run back-to-back, before the game resumes.
    using LuaDone = std::function<void(const std::vector<LuaValue>&)>;
    void RunLua(const LuaChunk& chunk, LuaDone done = nullptr);
    bool LuaPending();   // requests waiting in gameplay: the paused overlay lets single frames through for them
    int LayerNumber();   // emu thread: the room layout layer (SetLayerNumber), -1 if unknown
    // Emu thread, once per frame (s3eKeyboardUpdate), before queued Lua runs: gameplay = gameplay on top.
    void SetFrameTick(std::function<void(bool gameplay)> tick) { frame_tick_ = std::move(tick); }
    // Soft-key labels: 0 shown, 1 smart (hidden in gameplay only: Equip / Codec), 2 hidden everywhere.
    void SetHideSoftKeys(int mode) { hide_soft_keys_ = mode; }
    // Original debug commands (README §6.16): IwGx FPS counter, Lua InDebugMode() (Assert / Trace), IsFrameRateTest().
    void SetDebugCommands(bool show_fps, bool lua_debug_mode, bool frame_rate_test) {
        show_fps_ = show_fps; lua_debug_mode_ = lua_debug_mode; frame_rate_test_ = frame_rate_test;
    }
    // Emu thread: Lua Assert failures (with InDebugMode on), for the on-screen message list.
    void SetDebugMessageCallback(std::function<void(const std::string&)> cb) { debug_message_cb_ = std::move(cb); }
    void RefreshView();
    void ApplySoftKeyVisibility();   // emu thread, every frame: hides the soft-key labels/frames if enabled
    // Window focus (UI thread). With pause-on-focus-loss on, losing focus runs the game's OS PAUSE callback
    // (like an incoming call) and then freezes it (FocusPaused, see GLES pause hooks) until focus returns,
    // which runs UNPAUSE.
    void SetFocusLost(bool lost) { focus_lost_ = lost; }
    void SetPauseOnFocusLoss(bool on) { pause_on_focus_loss_ = on; }
    bool FocusPaused() const { return focus_paused_ && focus_lost_ && pause_on_focus_loss_; }
    void AddPausedTime(int64_t ms) { paused_ms_ += ms; }
    void StepFrame() { ++steps_; }   // photo mode (any thread): advance the frozen game clock by one update   // the overlay froze the game this long
    int64_t GameMs() const;   // emu thread: recompute the 3D view with the current aspect/FOV settings

    static void IntrHook(Cpu* uc, uint32_t intno, void* user_data);

private:
    S3ERuntime() = default;

    void HandleSyscall(Cpu* uc, uint32_t func_idx);

    Cpu* uc_ = nullptr;
    std::vector<std::string> imports_;
    GuestHeap heap_;
    void ZeroGuest(Cpu* uc, uint32_t addr, uint32_t len);
    struct { bool active; uint32_t due, fn, ud; } timer_{};
    // Guest callback queued to run when a syscall returns (one at a time; the return stub at
    // TRAMPOLINE_BASE + imports*8 resumes the interrupted syscall's return). prepare() runs right before
    // the call and may fill fn/args or decline (then on_done gets CB_SKIPPED); on_done gets the
    // callback's r0. Callbacks queued by on_done run back-to-back before the game resumes.
    struct PendingCallback {
        uint32_t fn, arg0, arg1;
        std::function<bool(PendingCallback&)> prepare;
        std::function<void(uint32_t)> on_done;
        uint32_t arg2 = 0, arg3 = 0;
    };
    static constexpr uint32_t CB_SKIPPED = 0xFFFFFFFFu;
    std::deque<PendingCallback> pending_cbs_;   // guest callbacks to run at the next syscall return
    std::function<void(uint32_t)> running_cb_done_;
    bool DispatchPending(Cpu* uc);         // starts the next queued callback; false if none ran
    void ApplyDisplay(Cpu* uc, bool notify);
    std::atomic<int> host_w_{480}, host_h_{640};
    std::atomic<bool> fixed_qvga_{false};
    std::atomic<bool> pillarbox_{false};
    uint32_t view_aspect_orig_ = 0;   // the game's 3D view aspect cap (4.12), read on first use
    uint32_t view_focal_orig_ = 0;    // the game's focal-length factor [params+0x70], read on first use
    std::atomic<int> fov_percent_{100};
    std::atomic<bool> draw_all_geometry_{false};
    std::mutex room_mutex_;
    std::string room_request_;              // pending level-select room (emu thread consumes it)
    bool room_change_active_ = false;       // our RoomChange call is running (room_hook substitutes the name)
    uint32_t room_name_buf_ = 0;           // guest buffer: the room name for room_hook
    std::string room_via_;
    uint32_t lua_seen_ = 0, lua_main_ = 0;  // a lua_State the game used; its main thread
    uint32_t LuaMainState(Cpu* uc);
    struct LuaRequest { std::string chunk; int nresults = 0; LuaDone done; };
    std::mutex lua_mutex_;
    std::deque<LuaRequest> lua_queue_;
    bool lua_busy_ = false;                 // emu thread: a request (or level select's chunks) is running
    uint32_t lua_buf_ = 0, lua_buf_cap_ = 0;   // guest buffer for the chunk and its name, reused
    std::function<void(bool)> frame_tick_;
    void ExecLua(Cpu* uc, const std::string& chunk, int nresults, LuaDone done);   // starts now
    void PumpLua(Cpu* uc);            // once per frame: starts the queued requests
    std::atomic<bool> in_gameplay_{false};  // InGameplay() as of the last frame end
    int gameplay_frames_ = 0;               // consecutive frames with gameplay on top
    std::atomic<int> hide_soft_keys_{0};
    std::atomic<bool> show_fps_{false}, lua_debug_mode_{false}, frame_rate_test_{false};
    std::function<void(const std::string&)> debug_message_cb_;
    void ApplyShowFps();
    std::unordered_map<std::string, std::string> icf_;   // "group/key" (lower case) -> value
    bool IcfLookup(const std::string& group, const std::string& key, std::string& out) const;
    std::atomic<int64_t> paused_ms_{0};
    std::atomic<bool> frozen_{false};       // photo mode: GameMs returns frozen_ms_
    std::atomic<int64_t> frozen_ms_{0};
    std::atomic<int> steps_{0};
    uint32_t photo_matrix_ = 0;             // guest CIwMat handed to IwGxSetViewMatrix in photo mode
    int64_t RunningMs() const;              // the clock without the photo-mode freeze
    void UpdatePhotoMode();
    // s3eDeviceRegister callbacks (S3E_DEVICE_PAUSE 0, UNPAUSE 1, EXIT 2, ...): id -> {fn, userdata}
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> device_cbs_;
    std::atomic<bool> focus_lost_{false}, pause_on_focus_loss_{false}, focus_paused_{false};
    bool focus_pause_sent_ = false;   // PAUSE delivered (or queued) and UNPAUSE not yet
    void UpdateFocusPause();          // emu thread, after each syscall
    std::vector<uint32_t> softkey_bars_;   // CIwGuiSoftkeys objects seen (0x4A08D4B4)
    std::vector<uint32_t> softkeys_hidden_; // label/frame elements we made invisible (restored outside gameplay)
    bool InGameplay();
    void PumpRoomChange(Cpu* uc);   // once per frame (s3eKeyboardUpdate): start a level-select request
    int surf_w_ = 240, surf_h_ = 320;           // guest screen (s3eSurfaceGetInt WIDTH/HEIGHT)
    bool display_init_ = false;
    std::vector<std::pair<uint32_t, uint32_t>> surface_cbs_; // s3eSurfaceRegister(1 = screen size): fn, userdata
    uint32_t orient_struct_ = 0;                // guest s3eSurfaceOrientation passed to those callbacks
    bool in_cb_ = false;
    uint32_t saved_[6]{}; // ret r0, r1, r2, r3, r12, lr
    std::mutex key_mutex_;
    // Keyboard (S3E key codes). The UI thread reports physical transitions (auto-repeat ignored) as
    // queued edges; s3eKeyboardUpdate snapshots them into frame_* which is all the game ever reads.
    bool phys_down_[256]{};
    bool pending_pressed_[256]{};
    bool pending_released_[256]{};
    bool frame_down_[256]{};
    bool frame_pressed_[256]{};
    bool frame_released_[256]{};
    bool swallowed_[256]{};   // held during s3eKeyboardClearState: invisible until physically released

    std::chrono::steady_clock::time_point start_time_;
    std::function<void(const uint16_t*, int, int)> frame_callback_;
    bool quit_requested_ = false;
    std::function<void()> exit_callback_;
    uint32_t frame_count_ = 0;

    // s3eSound: software mixer. Channels are driven by guest GEN_AUDIO callbacks (IwSound decodes and
    // resamples itself to s3eSoundGetInt(1) = SOUND_RATE) or play raw 16-bit PCM from guest memory.
    // PumpAudio (emu thread) builds SOUND_BLOCK-sample blocks into AudioOut whenever its ring runs low.
    static constexpr int MAX_SOUND_CHANNELS = 16;
    static constexpr int SOUND_RATE = 22050;
    static constexpr int SOUND_BLOCK = 512;
    struct SoundChannel {
        bool playing = false;
        bool paused = false;
        uint32_t rate = 8000;
        int32_t volume = 256;         // 0..256 (S3E_SOUND_MAX_VOLUME)
        uint32_t uservar = 0;
        uint32_t cb_fn[4]{}, cb_ud[4]{};   // END_SAMPLE, GEN_AUDIO, GEN_AUDIO_STEREO, STOP_AUDIO
        uint32_t start = 0, num_samples = 0;
        int32_t repeat = 1, loop_from = 0;
        double pos = 0;               // raw PCM playback position (source samples)
    } channels_[MAX_SOUND_CHANNELS];
    int32_t master_volume_ = 256;
    bool sound_started_ = false;
    bool mix_active_ = false;
    std::vector<int32_t> mix_acc_;    // host-mixed raw PCM channels for the current block
    uint32_t g_mix_buf_ = 0, g_gen_info_ = 0, g_end_info_ = 0;   // guest scratch (target, s3eSoundGenAudioInfo, end infos)
    void PumpAudio(Cpu* uc);
    void QueueGen(Cpu* uc, std::vector<int> chans, size_t i);
    void FinishMixBlock(Cpu* uc);
    void ChannelEnded(Cpu* uc, int ch);
    void MixRawChannel(Cpu* uc, int ch);

    // s3eAudio (compressed-stream playback): the game never calls it (UseS3EAudio is unset); state only.
    struct AudioStream {
        bool playing = false, paused = false;
        int32_t volume = 256;
        uint32_t cb_fn = 0, cb_ud = 0;
    } audio_;
};
