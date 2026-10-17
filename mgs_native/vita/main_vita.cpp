// PS Vita frontend (README.md §6.29): the game's code runs natively on the Cortex-A9 (cpu_native.cpp on kubridge),
// GL ES 1.1 goes to vitaGL (gles.cpp renders into a framebuffer object and blits it to the screen), the log to
// ux0:data/mgs_mobile/mgs_mobile.log and to port 17492 (tools/vita.py log). Single-threaded like the 3DS port: the
// game runs on the main thread, and each presented frame polls the buttons.
//
// Files: ux0:data/mgs_mobile/ holds the player's .n-gage package (any name), game/ (installed from it on the first
// start), mgs_mobile.ini, mgs_mobile.log and mgs_env.txt (optional debug variables, one NAME=value per line).
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

#include "audio.h"
#include "gamepad.h"
#include "gles.h"
#include "imgui.h"
#include "imgui_impl_gles1.h"
#include "installer.h"
#include "loader.h"
#include "overlay.h"
#include "platform.h"
#include "pngio.h"
#include "runtime.h"
#include "settings.h"
#include "textures.h"
#include "vfs.h"

// Our C/C++ heap (newlib) and the main thread's stack. The stack holds the guest's too: the system stops the
// process when a thread runs with its stack pointer outside its own stack, so cpu_native.cpp puts the guest
// stack (2 MB) in the lowest part of this one, below what our own code uses (~1 MB at most). The rest of the
// guest's memory comes from kubridge, and vitaGL takes most of what's left at startup (see kGuestReserve).
int _newlib_heap_size_user = 64 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
extern "C" struct _reent* __getreent(void);   // (newlib: this thread's C library state)
extern "C" volatile uint32_t g_native_exits, g_native_last_reason, g_native_last_pc;   // cpu_native.cpp

namespace {
constexpr int kW = 960, kH = 544;
// Memory vitaGL leaves alone: the guest's (64 MB heap, image + trampolines, stack, framebuffer; the installer's
// smaller sandbox comes and goes before it) plus a margin for threads and system buffers.
constexpr int kGuestReserve = 96 * 1024 * 1024;
// The helper threads (log, commands, watchdog) mostly wait: a higher priority than the main thread's (64 highest ..
// 191 lowest; the main thread has the default, 160) and a core of their own (the game on user core 0, the helpers
// on core 1), so the game's code keeping its core busy never holds them up.
constexpr int kHelperPriority = 96;
constexpr int kGameCore = SCE_KERNEL_CPU_MASK_USER_0, kHelperCore = SCE_KERNEL_CPU_MASK_USER_1;

// ---- log: file + one network client, fed by stdout / stderr / std::cout / std::cerr --------------------------
std::recursive_mutex g_log_mutex;
SceUID g_log_file = -1;
std::string g_file_pending;   // written to the card in batches (each sceIoWrite is a system call)
int g_client = -1;            // the connected PC, or -1
std::string g_backlog;        // everything logged so far, sent first when a PC connects, up to kBacklog
constexpr size_t kBacklog = 1024 * 1024;

void FlushLogFile() {
    std::lock_guard<std::recursive_mutex> lock(g_log_mutex);
    if (g_log_file >= 0 && !g_file_pending.empty()) sceIoWrite(g_log_file, g_file_pending.data(), g_file_pending.size());
    g_file_pending.clear();
}

void LogWrite(const char* p, size_t n) {
    std::lock_guard<std::recursive_mutex> lock(g_log_mutex);
    g_file_pending.append(p, n);
    static SceUInt64 last = 0;
    const SceUInt64 now = sceKernelGetProcessTimeWide();
    if (g_file_pending.size() > 16 * 1024 || now - last > 2000000) { FlushLogFile(); last = now; }
    if (g_backlog.size() + n <= kBacklog) g_backlog.append(p, n);
    if (g_client >= 0 && sceNetSend(g_client, p, n, 0) < 0) { sceNetSocketClose(g_client); g_client = -1; }
}

class LogBuf : public std::streambuf {
protected:
    int overflow(int c) override { if (c != EOF) { const char ch = static_cast<char>(c); LogWrite(&ch, 1); } return c; }
    std::streamsize xsputn(const char* s, std::streamsize n) override { LogWrite(s, static_cast<size_t>(n)); return n; }
};
LogBuf g_log_buf;

// Accepts one PC at a time; a new connection replaces the old one and gets the backlog.
int LogServerThread(SceSize, void*) {
    const int srv = sceNetSocket("log", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    SceNetSockaddrIn a{};
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(17492);
    if (srv < 0 || sceNetBind(srv, reinterpret_cast<SceNetSockaddr*>(&a), sizeof(a)) < 0 || sceNetListen(srv, 1) < 0) return 0;
    for (;;) {
        const int c = sceNetAccept(srv, nullptr, nullptr);
        if (c < 0) { sceKernelDelayThread(250 * 1000); continue; }
        std::lock_guard<std::recursive_mutex> lock(g_log_mutex);
        if (g_client >= 0) sceNetSocketClose(g_client);
        sceNetSend(c, g_backlog.data(), g_backlog.size(), 0);
        g_client = c;
    }
}

int CommandThread(SceSize, void*);

// Every 5 s while the game's code runs without presenting a frame (installs, loads): how many times it returned
// to ours, why last, and from where, to tell slow progress from a hang. Logged only when no frame came. And the
// frame rate over each 30 s with frames.
std::atomic<uint32_t> g_frames{0};
int WatchdogThread(SceSize, void*) {
    uint32_t frames = 0, exits = 0, frames_30s = 0, ticks = 0;
    for (;;) {
        sceKernelDelayThread(5 * 1000 * 1000);
        const uint32_t f = g_frames, e = g_native_exits;
        if (++ticks % 6 == 0) {
            if (f != frames_30s) std::cout << "[Perf] " << (f - frames_30s) / 30.0f << " fps (last 30 s)" << std::endl;
            frames_30s = f;
        }
        if (f == frames)
            std::cout << "[Watch] no frame for 5 s: " << e - exits << " exits to the host (" << e << " in all), last reason 0x"
                      << std::hex << g_native_last_reason << " at pc 0x" << g_native_last_pc << std::dec << std::endl;
        frames = f; exits = e;
    }
}

void StartLog(const std::string& dir) {
    sceIoMkdir(dir.substr(0, dir.size() - 1).c_str(), 0777);
    g_log_file = sceIoOpen((dir + "mgs_mobile.log").c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    g_backlog.reserve(64 * 1024);
    // printf / fprintf to the log. Newlib sets the standard streams up on first use (which would undo a reassigned
    // stdout), so the main thread's are pointed at the log in place: the game and its crash reports run on it, the
    // helper threads log through std::cout. Two sets: libc's own calls (printf) find this thread's through
    // __getreent(), while our code's stdout / stderr resolve through _impure_ptr. Line-buffered: one log write
    // per line.
    struct _reent* const reents[] = {_impure_ptr, __getreent()};
    for (struct _reent* r : reents) {
        _fflush_r(r, r->_stdout);   // (sets the streams up)
        for (FILE* f : {r->_stdout, r->_stderr}) {
            setvbuf(f, nullptr, _IOLBF, 1024);
            f->_write = [](struct _reent*, void*, const char* p, _READ_WRITE_BUFSIZE_TYPE n) -> _READ_WRITE_RETURN_TYPE {
                LogWrite(p, static_cast<size_t>(n));
                return n;
            };
        }
    }
    std::cout.rdbuf(&g_log_buf);
    std::cerr.rdbuf(&g_log_buf);

    static char net_mem[1024 * 1024];
    SceNetInitParam p{net_mem, sizeof(net_mem), 0};
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0 || sceNetInit(&p) < 0 || sceNetCtlInit() < 0) return;
    const SceUID t = sceKernelCreateThread("log_server", LogServerThread, kHelperPriority, 0x4000, 0, kHelperCore, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
    const SceUID cmd = sceKernelCreateThread("commands", CommandThread, kHelperPriority, 0x4000, 0, kHelperCore, nullptr);
    if (cmd >= 0) sceKernelStartThread(cmd, 0, nullptr);
    const SceUID watch = sceKernelCreateThread("watchdog", WatchdogThread, kHelperPriority, 0x4000, 0, kHelperCore, nullptr);
    if (watch >= 0) sceKernelStartThread(watch, 0, nullptr);
    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) std::cout << "Log: " << info.ip_address << ":17492" << std::endl;
}

// Screenshots for tools/vita.py shot: a PC connects to port 17493 and sends "shot"; the next frame is read back
// before it goes to the screen, saved as ux0:data/mgs_mobile/shot.png, and the PC gets "ok <path>" (then fetches
// it over FTP). The system screenshot (PS + START) would also press START in the game.
std::atomic<int> g_shot_client{-1};

// "press cross+up 500": those buttons held for that many ms (default 200), as if pressed on the Vita (tools/vita.py
// press). vitacompanion's simulated buttons often never reached sceCtrlPeekBufferPositive.
std::string PressFromCommand(const char* args) {
    static const struct { const char* name; uint32_t bit; } kButtons[] = {
        {"cross", SCE_CTRL_CROSS}, {"circle", SCE_CTRL_CIRCLE}, {"square", SCE_CTRL_SQUARE}, {"triangle", SCE_CTRL_TRIANGLE},
        {"up", SCE_CTRL_UP}, {"down", SCE_CTRL_DOWN}, {"left", SCE_CTRL_LEFT}, {"right", SCE_CTRL_RIGHT},
        {"l", SCE_CTRL_LTRIGGER}, {"r", SCE_CTRL_RTRIGGER}, {"start", SCE_CTRL_START}, {"select", SCE_CTRL_SELECT} };
    char names[48] = {};
    int ms = 200;
    if (sscanf(args, "%47s %d", names, &ms) < 1) return "error: press <buttons> [ms]\n";
    uint32_t mask = 0;
    for (char* tok = strtok(names, "+"); tok; tok = strtok(nullptr, "+")) {
        bool found = false;
        for (const auto& b : kButtons) if (strcmp(tok, b.name) == 0) { mask |= b.bit; found = true; }
        if (!found) return std::string("error: unknown button ") + tok + "\n";
    }
    Gamepad::Inject(mask, ms);
    return "ok\n";
}

int CommandThread(SceSize, void*) {
    const int srv = sceNetSocket("cmd", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    SceNetSockaddrIn a{};
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(17493);
    if (srv < 0 || sceNetBind(srv, reinterpret_cast<SceNetSockaddr*>(&a), sizeof(a)) < 0 || sceNetListen(srv, 1) < 0) return 0;
    for (;;) {
        const int c = sceNetAccept(srv, nullptr, nullptr);
        if (c < 0) { sceKernelDelayThread(250 * 1000); continue; }
        char line[64] = {};
        const int n = sceNetRecv(c, line, sizeof(line) - 1, 0);
        if (n > 0 && strncmp(line, "shot", 4) == 0 && g_shot_client < 0) { g_shot_client = c; continue; }   // (answered by Capture)
        if (n > 0 && strncmp(line, "press ", 6) == 0) {
            const std::string reply = PressFromCommand(line + 6);
            sceNetSend(c, reply.data(), reply.size(), 0);
            sceNetSocketClose(c);
            continue;
        }
        static const char kUnknown[] = "error: unknown command (shot, press <buttons> [ms])\n";
        sceNetSend(c, kUnknown, sizeof(kUnknown) - 1, 0);
        sceNetSocketClose(c);
    }
}

void Capture() {   // the frame about to be swapped (default framebuffer), to shot.png
    const int c = g_shot_client.exchange(-1);
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::vector<uint32_t> px(kW * kH), out(px.size());
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
    for (int y = 0; y < kH; ++y)   // bottom-up RGBA bytes -> top-down 0xAARRGGBB
        for (int x = 0; x < kW; ++x) {
            const uint32_t p = px[static_cast<size_t>(kH - 1 - y) * kW + x];
            out[static_cast<size_t>(y) * kW + x] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
        }
    const std::string path = Platform::U8String(Platform::ExeDir()) + "shot.png";
    const std::string err = PngIO::Write(Platform::U8Path(path), out.data(), kW, kH);
    const std::string reply = err.empty() ? "ok " + path + "\n" : "error: " + err + "\n";
    sceNetSend(c, reply.data(), reply.size(), 0);
    sceNetSocketClose(c);
}

void SwapBuffers() {
    ++g_frames;
    if (g_shot_client >= 0) Capture();
    vglSwapBuffers(GL_FALSE);
}

void LoadEnvFile(const std::string& dir) {   // debug variables (README.md §7)
    FILE* env = fopen((dir + "mgs_env.txt").c_str(), "r");
    if (!env) return;
    char line[512];
    while (fgets(line, sizeof line, env)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq != std::string::npos && eq > 0 && l[0] != '#') {
            setenv(l.substr(0, eq).c_str(), l.substr(eq + 1).c_str(), 1);
            std::cout << "[Env] " << l << std::endl;
        }
    }
    fclose(env);
}

// Resets the system's idle timers (standby, screen dimming and off), as a button press does: the game is often
// watched without touching anything (cutscenes, codec calls, the setup, tests driven from a PC).
void KeepAwake() { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }

// ---- message screen (setup progress, errors): Dear ImGui on vitaGL, before and outside the game --------------
ImGuiContext* g_screen_ctx = nullptr;

void ShowScreen(const std::string& text, float progress = -1.0f) {
    KeepAwake();
    if (!g_screen_ctx) {
        g_screen_ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().FontScaleMain = 2.0f;
        ImGui_ImplGLES1_Init();
    }
    ImGui::SetCurrentContext(g_screen_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(kW, kH);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(kW / 2.0f, kH / 2.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kW * 0.8f, 0));
    ImGui::Begin("##screen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextWrapped("%s", text.c_str());
    if (progress >= 0.0f) { ImGui::Spacing(); ImGui::ProgressBar(progress, ImVec2(-1, 0)); }
    ImGui::End();
    ImGui::Render();
    glViewport(0, 0, kW, kH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplGLES1_RenderDrawData(ImGui::GetDrawData());
    SwapBuffers();
}

void CloseScreen() {   // before the game: the F1 overlay makes its own ImGui context
    if (!g_screen_ctx) return;
    ImGui::SetCurrentContext(g_screen_ctx);
    ImGui_ImplGLES1_Shutdown();
    ImGui::DestroyContext(g_screen_ctx);
    g_screen_ctx = nullptr;
}

// Shows a message (and logs it) until START is pressed.
void WaitForExit(const std::string& message) {
    std::cout << message << std::endl;
    FlushLogFile();
    bool was_down = true;   // (a START still held from the game doesn't count)
    for (;;) {
        ShowScreen(message + "\n\nPress START to exit.");
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const bool down = (pad.buttons & SCE_CTRL_START) != 0;
        if (down && !was_down) break;
        was_down = down;
    }
}

// First run (no game/ yet): installs from a *.n-gage in ux0:data/mgs_mobile/, whatever its name, trying them in
// name order until one is this game (another game's package fails the check before anything is written). The
// archives are unpacked by the game's own code on the native CPU (installer.cpp); no PNG textures. Then the app
// starts itself again (a fresh process for the game). Returns only when there is nothing to play.
void InstallFromPackage(const std::string& dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> packages;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::u8path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string ext = it->path().extension().u8string();
        for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (ext == ".n-gage" && it->is_regular_file(ec)) packages.push_back(it->path());
    }
    std::sort(packages.begin(), packages.end());
    if (packages.empty()) {
        WaitForExit("No game files yet.\n\nCopy your Metal Gear Solid Mobile package (the .n-gage file, any name) to\n" + dir +
                    "\nand start the game again.");
        return;
    }
    std::string errors;
    for (const fs::path& p : packages) {
        const std::string name = p.filename().u8string();
        std::string error;
        int last_pct = -1;
        const bool ok = Installer::Install(p, dir, [&](float f, const std::string& status) {
            const int pct = static_cast<int>(f * 100.0f);
            if (pct == last_pct && status.find('(') != std::string::npos) return;   // (per-file steps: on percent changes)
            last_pct = pct;
            ShowScreen("Setting up Metal Gear Solid Mobile\nfrom " + name + "\n\n" + status, f);
        }, &error);
        if (ok) {
            std::cout << "[Setup] Done: starting again" << std::endl;
            FlushLogFile();
            ShowScreen("Setup complete. Starting the game...");
            sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
            return;   // (not reached)
        }
        errors += name + ": " + error + "\n";
    }
    WaitForExit("Setup failed.\n\n" + errors);
}

// ---- the game -------------------------------------------------------------------------------------------------
bool g_pad_keys[256];
void SetPadKeys(const bool keys[256]) {
    for (uint32_t k = 0; k < 256; ++k)
        if (g_pad_keys[k] != keys[k]) {
            g_pad_keys[k] = keys[k];
            S3ERuntime::Instance().SetKeyState(k, keys[k]);
        }
}

void ApplySettings() {
    Settings& s = Settings::Instance();
    S3ERuntime& rt = S3ERuntime::Instance();
    rt.SetFixedQVGA(!s.arbitrary_resolution);
    rt.SetPillarbox(!s.arbitrary_resolution);
    rt.SetFovPercent(s.fov_percent);
    rt.SetHideSoftKeys(s.hide_soft_keys);
    GLES::Instance().SetTextureFiltering(s.texture_filtering);
    TexturePack::SetEnabled(s.custom_textures);
    GLES::Instance().SetAntiAliasing(0, 1);   // (not tried on vitaGL yet)
    GLES::Instance().SetNativeResolution(false);
    rt.SetDrawAllGeometry(s.draw_all_geometry);
    rt.SetPauseOnFocusLoss(false);   // (the system suspends the whole app on the PS button)
    const bool dbg = s.debug_mode;
    rt.SetDebugCommands(dbg && s.dbg_show_fps, dbg && s.dbg_lua_debug_mode, dbg && s.dbg_frame_rate_test);
}

bool g_close_overlay = false;   // asked from inside the overlay (applied between frames)

void SetOverlayOpen(bool open) {
    Overlay& o = Overlay::Instance();
    if (open == o.IsOpen()) return;
    if (open) { const bool none[256] = {}; SetPadKeys(none); }   // the game shouldn't see keys held while it opens
    o.SetOpen(open);
    if (!open) { Settings::Instance().Save(); std::cout << "[Menu] Closed (Select opens it)" << std::endl; }
}

void FrameTick() {   // once per presented frame (GLES::Host::swap)
    KeepAwake();
    Gamepad::Instance().Poll();
    if (g_close_overlay) { g_close_overlay = false; SetOverlayOpen(false); }
}

void LogMemory(const char* when) {
    SceKernelFreeMemorySizeInfo m{};
    m.size = sizeof(m);
    sceKernelGetFreeMemorySize(&m);
    std::cout << "[Vita] Free memory (" << when << "): user " << m.size_user / 1024 << " KB, cdram " << m.size_cdram / 1024
              << " KB, phycont " << m.size_phycont / 1024 << " KB" << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = Platform::U8String(Platform::ExeDir());
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), kGameCore);
    StartLog(dir);
    std::cout << "MGS Mobile - Vita port, built " << __DATE__ << " " << __TIME__ << std::endl;
    std::cout << "[Vita] Main thread: priority " << sceKernelGetThreadCurrentPriority() << ", cores 0x" << std::hex
              << sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()) << std::dec << std::endl;
    scePowerSetArmClockFrequency(444);   // the game's code and ours on one core: the fastest clock
    scePowerSetGpuClockFrequency(222);
    LoadEnvFile(dir);
    LogMemory("start");
    vglInitExtended(0, kW, kH, kGuestReserve, SCE_GXM_MULTISAMPLE_NONE);
    LogMemory("after vitaGL");

    Settings& settings = Settings::Instance();
    settings.Load();
    settings.debug_mode = getenv("MGS_VERBOSE") != nullptr;   // the verbose log only on request (README.md §7)

    if (!Installer::Installed(dir)) {
        InstallFromPackage(dir);
        sceKernelExitProcess(0);
        return 0;
    }
    ShowScreen("Loading...");
    VirtualFileSystem::Instance().SetAssetRoot(dir + "game/data");
    // All of game/data in memory (one pack file, game/data.pak, built on the first start): room changes then
    // don't wait for the memory card. MGS_NO_PACK=1: the files one by one.
    const std::string pack = getenv("MGS_NO_PACK") ? "" : dir + "game/data.pak";
    VirtualFileSystem::Instance().PreloadAll(true, pack, [] { ShowScreen("Preparing the game data\n(first start only)"); });
    TexturePack::Init(Platform::ExeDir() / "textures");   // custom textures, if a textures folder was copied too

    S3EBinary binary;
    if (!LoadS3E(dir + "game/mgs_ngi", binary)) {
        WaitForExit("Could not load " + dir + "game/mgs_ngi.");
        sceKernelExitProcess(0);
        return 1;
    }
    std::string cpu_error;
    std::unique_ptr<Cpu> cpu = Cpu::CreateNative(&cpu_error);
    if (!cpu || !SetupSandbox(cpu.get(), binary, ImageBase())) {
        WaitForExit("Could not set up the game's memory. " + cpu_error + "\n\nIs kubridge.skprx in ur0:tai/config.txt?");
        sceKernelExitProcess(0);
        return 1;
    }
    LogMemory("game memory set up");
    CloseScreen();

    S3ERuntime& rt = S3ERuntime::Instance();
    rt.Init(cpu.get(), binary.imports);
    rt.SetIcf(binary.config);
    rt.SetHostClientSize(kW, kH);
    ApplySettings();
    rt.SetFrameCallback([](const uint16_t* pixels, int w, int h) { GLES::Instance().PresentSoftware(pixels, w, h); });
    GLES::Instance().SetFrameCallback([](const uint32_t*, int, int) {});
    GLES::Host host;
    host.make_current = [] { return true; };   // (vitaGL is up since startup)
    host.get_proc = [](const char* name) { return reinterpret_cast<void*>(vglGetProcAddress(name)); };
    host.window_size = [](int& w, int& h) { w = kW; h = kH; };
    host.swap = [] { SwapBuffers(); FrameTick(); };
    GLES::SetHost(std::move(host));
    rt.SetExitCallback([] {});
    // F1 overlay over the game: drawn after each game frame while open, which pauses the game until it closes.
    GLES::Instance().SetOverlayCallback([](int w, int h) { Overlay::Instance().Render(w, h); });
    GLES::Instance().SetPauseHooks(
        [] {
            S3ERuntime& r = S3ERuntime::Instance();
            return Overlay::Instance().IsOpen() && !r.LuaPending() && !r.IsQuitRequested();
        },
        [](long long ms) { S3ERuntime::Instance().AddPausedTime(ms); });
    Overlay::Instance().SetApplyCallback([] {
        ApplySettings();
        S3ERuntime::Instance().RefreshView();
    });
    Overlay::Instance().SetCloseCallback([] { g_close_overlay = true; });
    Overlay::Instance().SetLevelSelect([] { return S3ERuntime::Instance().CanChangeRoom(); },
                                       [](const std::string& room, const std::string& via) { S3ERuntime::Instance().RequestRoomChange(room, via); });
    GLES::SetWarningCallback([](const std::string& w) { Overlay::Instance().AddWarning("[GLES] " + w); });
    Gamepad::Instance().Start(SetPadKeys, nullptr,
                              [] { return Overlay::Instance().IsOpen(); },                       // raw input to the overlay
                              [](const Gamepad::Raw& raw) { Overlay::Instance().OnPad(raw); },
                              [] { SetOverlayOpen(!Overlay::Instance().IsOpen()); });          // Select

    const uint32_t entry = G(binary.entry_point);
    std::cout << "[EmuThread] Starting execution at 0x" << std::hex << entry << std::dec << "..." << std::endl;
    const int err = cpu->Start(entry, 0xDEADBEEF);
    if (err != Cpu::OK && !rt.IsQuitRequested()) {
        uint32_t pc = 0, lr = 0;
        cpu->RegRead(Cpu::REG_PC, &pc);
        cpu->RegRead(Cpu::REG_LR, &lr);
        char where[64];
        snprintf(where, sizeof where, " at PC=0x%x, LR=0x%x", static_cast<unsigned>(pc), static_cast<unsigned>(lr));
        rt.DumpExitDiagnostics(true);   // [Hist] import counts, [Recent] the last syscalls (log only)
        WaitForExit("Emulation stopped: " + std::string(cpu->ErrorText(err)) + where + "\n\nThe log is in " + dir + "mgs_mobile.log.");
    } else {
        std::cout << "[EmuThread] Emulation stopped cleanly." << std::endl;
    }
    AudioOut::Instance().Stop();
    settings.Save();
    rt.DumpExitDiagnostics();
    FlushLogFile();
    sceKernelExitProcess(0);
    return 0;
}
