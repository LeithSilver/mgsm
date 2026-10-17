// Nintendo 3DS frontend (README.md §6.28): the game's code runs natively on the ARM11 (cpu_native.cpp), GL ES 1.1
// goes to citro3d (gl3ds.cpp) on the top screen, the log to the bottom screen. Single-threaded: the game runs on
// the main thread, and each presented frame polls the buttons and the system (HOME menu, sleep, close).
//
// Files: sdmc:/3ds/mgs_mobile/ holds game/ (installed on a PC with `mgs_mobile --install`, then copied over),
// mgs_mobile.ini, mgs_mobile.log, and mgs_env.txt (optional debug variables, one NAME=value per line).
// Started through 3dslink (`3dslink -a <3ds ip> mgs_mobile.3dsx`), the log also goes back to the PC.
#include <3ds.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <algorithm>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "gl3ds.h"
#include "audio.h"
#include "gamepad.h"
#include "gles.h"
#include "overlay.h"
#include "installer.h"
#include "loader.h"
#include "platform.h"
#include "runtime.h"
#include "settings.h"
#include "textures.h"
#include "vfs.h"

extern "C" bool g_profile;                    // cpu_native.cpp: MGS_PROFILE=1 times every syscall
extern "C" unsigned long long g_host_ticks;   // cpu_native.cpp: time in the runtime (syscalls, hooks)
extern "C" unsigned g_code_flushes;           // cpu_native.cpp: instruction cache flushes
extern "C" unsigned long long g_svc_ticks[512];   // cpu_native.cpp: per import, time and calls
extern "C" unsigned g_svc_calls[512];
u32 __stacksize__ = 1024 * 1024;   // host code (runtime, GL) runs on the main thread's stack
// libctru's heaps get sizes of their own instead of all the application memory: the rest is mapped for the guest
// (cpu_native.cpp Reserve), page by page with gaps, outside our own malloc heap. Sized per console by
// __system_allocateHeaps below: measured peaks (New 3DS, README.md section 6.28) C heap 8.0 MB (3.2 MB of it the
// preloaded game data), linear 10.0 MB, guest heap 35.3 MB.
u32 __ctru_heap_size = 28 * 1024 * 1024;          // our C/C++ heap (incl. the game data in RAM)
u32 __ctru_linear_heap_size = 24 * 1024 * 1024;   // GPU: textures, vertex arenas, render targets, sound
// Old 3DS / 2DS (64 MB of application memory): small heaps, no game-data preload, no stereo, so the guest
// (~40 MB: heap, image, stack, framebuffer) fits in what's left.
bool g_small_memory = false;

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_linear_heap;

// libctru's own (2.7.0, system/allocateHeaps.c) with the sizes picked here, before main and the C++ constructors.
void __system_allocateHeaps(void) {
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    s64 max_commit = 0, current_commit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&current_commit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    const u32 remaining = static_cast<u32>(max_commit - current_commit) & ~0xFFFu;
    if (remaining < 80u * 1024 * 1024) {   // Old 3DS / 2DS: ~62 MB here; New 3DS ~120 MB
        g_small_memory = true;
        __ctru_heap_size = 7 * 1024 * 1024;           // peak ~4.8 MB without the preload
        __ctru_linear_heap_size = 11 * 1024 * 1024;   // peak ~9.2 MB without the right-eye target
    }
    if (__ctru_heap_size + __ctru_linear_heap_size > remaining) svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC,
                                  static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}

namespace {
// ---- log: bottom-screen console + 3dslink socket + mgs_mobile.log -------------------------------------------
const devoptab_t* g_console = nullptr;
int g_link_sock = -1;
FILE* g_log_file = nullptr;
std::recursive_mutex g_log_mutex;

std::string g_backlog;   // everything logged so far (sent first when a PC connects), up to kBacklog
constexpr size_t kBacklog = 1024 * 1024;

bool g_console_hidden = false;   // the F1 overlay has the bottom screen
// The log on the bottom screen too: only with MGS_VERBOSE, and for an error before exit (WaitForExit). Otherwise
// the bottom screen stays blank, and the log goes to the SD card and the network only.
bool g_console_log = false;

ssize_t TeeWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(g_log_mutex);
    if (g_console && g_console_log && !g_console_hidden) g_console->write_r(r, fd, ptr, len);   // (not over the F1 overlay)
    if (g_link_sock >= 0 && send(g_link_sock, ptr, len, 0) < 0) { close(g_link_sock); g_link_sock = -1; }
    if (g_backlog.size() + len <= kBacklog) g_backlog.append(ptr, len);
    if (g_log_file) {   // buffered: flushed every 2 s, before waiting on an error, and by the crash handler
        fwrite(ptr, 1, len, g_log_file);
        static u64 last = 0;
        const u64 now = svcGetSystemTick();
        if (now - last > 2000 * static_cast<u64>(CPU_TICKS_PER_MSEC)) { fflush(g_log_file); last = now; }
    }
    return static_cast<ssize_t>(len);
}
devoptab_t g_tee = {};

u32* g_soc_buffer = nullptr;

// The log over the network: a PC connects to port 17492 at any time (the 3DS listens, so a PC firewall doesn't
// get in the way) and gets everything logged so far, then the rest live. A thread waits for the connection.
void LogServer(void*) {
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(17492);
    a.sin_addr.s_addr = INADDR_ANY;
    if (srv < 0 || bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(srv, 1) != 0) return;
    // Polled, never a blocking accept: all socket calls share one session with the SOC service, and a blocked
    // accept would hold up the game thread's send until a PC connects.
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL, 0) | O_NONBLOCK);
    for (;;) {
        const int c = accept(srv, nullptr, nullptr);
        if (c < 0) { svcSleepThread(250 * 1000000LL); continue; }
        fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
        std::lock_guard<std::recursive_mutex> lock(g_log_mutex);
        if (g_link_sock >= 0) close(g_link_sock);
        send(c, g_backlog.data(), g_backlog.size(), 0);
        g_link_sock = c;
    }
}

void StartLog() {
    consoleInit(GFX_BOTTOM, nullptr);
    g_console = devoptab_list[STD_OUT];
    g_soc_buffer = static_cast<u32*>(memalign(0x1000, 0x40000));
    const bool net = g_soc_buffer && R_SUCCEEDED(socInit(g_soc_buffer, 0x40000));
    if (net) {
        g_backlog.reserve(64 * 1024);
        threadCreate(LogServer, nullptr, 16 * 1024, 0x30, -2, true);
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/mgs_mobile", 0777);
    g_log_file = fopen("sdmc:/3ds/mgs_mobile/mgs_mobile.log", "w");
    if (g_log_file) setvbuf(g_log_file, nullptr, _IOFBF, 64 * 1024);
    g_tee = *g_console;
    g_tee.name = "tee";
    g_tee.write_r = TeeWrite;
    devoptab_list[STD_OUT] = &g_tee;
    devoptab_list[STD_ERR] = &g_tee;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // (after the tee: printed earlier, it went straight to the bottom screen)
    if (net) printf("Log: %s:17492\n", inet_ntoa(in_addr{static_cast<in_addr_t>(gethostid())}));
}

// Straight to the console, past the tee (and so not into the log).
void ConsoleWrite(const char* s) {
    std::lock_guard<std::recursive_mutex> lock(g_log_mutex);
    if (g_console) g_console->write_r(_REENT, nullptr, s, strlen(s));
}


// The newest Luma3DS ARM11 crash dump (sdmc:/luma/dumps/arm11/), logged once at startup: registers, the words
// at the top of the stack (return addresses to look up in the build's .elf) and the process name. A crash
// outside the game's code (our own, another thread) ends there instead of in the log.
void LogLastCrashDump() {
    std::string newest;
    for (int n = 99; n >= 0 && newest.empty(); --n) {
        char path[64];
        snprintf(path, sizeof path, "sdmc:/luma/dumps/arm11/crash_dump_%08d.dmp", n);
        if (FILE* f = fopen(path, "rb")) { fclose(f); newest = path; }
    }
    if (newest.empty()) return;
    FILE* f = fopen(newest.c_str(), "rb");
    std::vector<uint8_t> d(64 * 1024);
    d.resize(fread(d.data(), 1, d.size(), f));
    fclose(f);
    auto u32at = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; };
    if (d.size() < 40) return;
    const uint32_t type = u32at(12), regs = u32at(20), code = u32at(24), stack = u32at(28);
    static const char* kNames[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
                                   "sp", "lr", "pc", "cpsr", "dfsr", "ifsr", "far", "fpexc", "fpinst", "fpinst2"};
    std::cout << "[CrashDump] " << newest << ": type " << type << std::hex;
    for (uint32_t i = 0; i < regs / 4 && i < 23; ++i) std::cout << (i % 6 ? " " : "\n[CrashDump]  ") << kNames[i] << "=" << u32at(40 + 4 * i);
    std::cout << "\n[CrashDump]  stack:";
    const size_t s0 = 40 + regs + code;
    for (uint32_t i = 0; i < stack / 4 && i < 48; ++i) std::cout << (i % 8 ? " " : "\n[CrashDump]   ") << u32at(s0 + 4 * i);
    const size_t extra = s0 + stack;
    if (extra + 8 <= d.size()) std::cout << "\n[CrashDump]  process " << std::string(reinterpret_cast<const char*>(&d[extra]), 8).c_str();
    std::cout << std::dec << std::endl;
}

void LoadEnvFile() {   // debug variables (README.md §7)
    FILE* env = fopen("sdmc:/3ds/mgs_mobile/mgs_env.txt", "r");
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

// Shows an error on the bottom screen (and logs it), then waits for START (or the HOME menu's close).
void WaitForExit(const std::string& message) {
    g_console_hidden = false;
    if (!g_console_log) { g_console_log = true; ConsoleWrite("\x1b[2J"); }
    std::cout << message << "\n\nPress START to exit." << std::endl;
    if (g_log_file) fflush(g_log_file);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

// ---- input --------------------------------------------------------------------------------------------------
bool g_pad_keys[256];
void SetPadKeys(const bool keys[256]) {
    for (uint32_t k = 0; k < 256; ++k)
        if (g_pad_keys[k] != keys[k]) { g_pad_keys[k] = keys[k]; S3ERuntime::Instance().SetKeyState(k, keys[k]); }
}

void ApplySettings() {
    Settings& s = Settings::Instance();
    S3ERuntime& rt = S3ERuntime::Instance();
    rt.SetFixedQVGA(false);   // the top screen's 400x240 (5:3)
    rt.SetPillarbox(false);
    rt.SetFovPercent(s.fov_percent);
    rt.SetHideSoftKeys(s.hide_soft_keys);
    GLES::Instance().SetTextureFiltering(s.texture_filtering);
    TexturePack::SetEnabled(s.custom_textures);
    GLES::Instance().SetAntiAliasing(0, 1);
    GLES::Instance().SetNativeResolution(false);
    rt.SetDrawAllGeometry(true);   // the 400 px wide view shows past the game's culling (README.md section 6.9)
    rt.SetPauseOnFocusLoss(true);   // HOME menu / sleep: like a call on the phone
    const bool dbg = s.debug_mode;
    rt.SetDebugCommands(dbg && s.dbg_show_fps, dbg && s.dbg_lua_debug_mode, dbg && s.dbg_frame_rate_test);
}

Cpu* g_cpu = nullptr;

// Once per presented frame (GLES::Host::swap, after the frame went to the screen): buttons and the system.
void LogMemory(const char* when) {
    const struct mallinfo mi = mallinfo();
    std::cout << "[3DS] Memory (" << when << "): C heap " << mi.uordblks / 1024 << " KB used of " << envGetHeapSize() / 1024
              << " KB, linear " << linearSpaceFree() / 1024 << " KB free" << std::endl;
}

// Our heap running out ends in an abort (a Luma crash dump, no message): say so first.
void OutOfMemory() {
    std::set_new_handler(nullptr);
    g_console_hidden = false;
    g_console_log = true;   // (on the bottom screen too: the app stops here)
    std::cout << "[3DS] Out of memory (operator new)" << std::endl;
    LogMemory("out of memory");
    if (g_log_file) fflush(g_log_file);
    for (;;) svcSleepThread(1000000000LL);
}

// ---- F1 overlay on the bottom screen --------------------------------------------------------------------------
// Select opens and closes it; the game pauses meanwhile (gles.cpp), its buttons drive the overlay (cursor, A to
// click) and the touch screen is the mouse. Closed, the bottom screen is the log console again.
bool g_close_overlay = false;   // asked from inside the overlay (applied between frames)

// The console's screen back (format, single buffering, cleared). Only the first consoleInit installs libctru's
// stdout devoptab, so stdout stays our tee and g_console the console's devoptab from StartLog: reading
// devoptab_list[STD_OUT] here would give the tee itself, and TeeWrite then called itself until the stack ran out
// (the hang on closing the overlay, which took the HOME menu with it).
void RestoreConsole() {
    consoleInit(GFX_BOTTOM, nullptr);   // (clears the screen)
}

void SetOverlayOpen(bool open) {
    Overlay& o = Overlay::Instance();
    if (open == o.IsOpen()) return;
    if (open) { const bool none[256] = {}; SetPadKeys(none); }   // the game shouldn't see keys held while it opens
    o.SetOpen(open);
    g_console_hidden = open;
    GL3DS::SetBottomScreen(open);
    if (!open) { RestoreConsole(); Settings::Instance().Save(); std::cout << "[Menu] Closed (Select opens it)" << std::endl; }
}

void PollTouch() {   // the touch screen as the overlay's mouse (bottom screen pixels = overlay pixels)
    static bool down = false;
    const bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
    if (!Overlay::Instance().IsOpen()) { down = false; return; }
    if (touching) {
        touchPosition t;
        hidTouchRead(&t);
        Overlay::Instance().OnMouseMove(static_cast<float>(t.px), static_cast<float>(t.py));
    }
    if (touching != down) { Overlay::Instance().OnMouseButton(0, touching); down = touching; }
}

void PollInput() {
    Gamepad::Instance().Poll();
    PollTouch();
    if (g_close_overlay) { g_close_overlay = false; SetOverlayOpen(false); }
}

bool g_perf_log = false;   // the [Perf] and memory lines every 300 frames: MGS_VERBOSE or MGS_PROFILE only

// High-water marks of the three pools (to size them for the Old 3DS, README.md section 6.28): our C/C++ heap, the
// linear heap (GPU: textures, vertex arenas, render targets, sound) and the guest heap. Checked every 60 frames;
// a [Mem] line in the log (not on screen) only when one of them grew by 128 KB or more since the last line.
void TrackMemoryPeaks() {
    static uint32_t c_peak = 0, linear_peak = 0, guest_peak = 0, logged = 0;
    const struct mallinfo mi = mallinfo();
    c_peak = std::max<uint32_t>(c_peak, mi.uordblks);
    linear_peak = std::max<uint32_t>(linear_peak, envGetLinearHeapSize() - linearSpaceFree());
    guest_peak = std::max<uint32_t>(guest_peak, S3ERuntime::Instance().GuestHeapPeak());
    const uint32_t sum = c_peak + linear_peak + guest_peak;
    if (sum < logged + 128 * 1024) return;
    logged = sum;
    std::cout << "[Mem] Peak: C heap " << c_peak / 1024 << " KB of " << envGetHeapSize() / 1024 << ", linear "
              << linear_peak / 1024 << " KB of " << envGetLinearHeapSize() / 1024 << ", guest heap " << guest_peak / 1024
              << " KB" << std::endl;
}

void FrameTick() {
    static unsigned frames = 0;
    static u64 t_last = svcGetSystemTick();
    static unsigned mem_frames = 0;
    if (++mem_frames % 60 == 0) TrackMemoryPeaks();
    if (g_perf_log && ++frames % 300 == 0) {   // timing: per frame, averaged over the last 300
        const u64 now = svcGetSystemTick();
        const double frame_ms = (now - t_last) / CPU_TICKS_PER_MSEC / 300;
        t_last = now;
        double rec, wait, end;
        GL3DS::TakeStats(rec, wait, end);
        const double host = g_host_ticks / CPU_TICKS_PER_MSEC / 300;
        g_host_ticks = 0;
        // game: the guest's own code; runtime: syscalls and hooks (incl. gles.cpp and the 60 fps pacing sleep);
        // record: gl3ds turning draws into vertices; present: EndFrame (replay), of which GPU wait.
        rec /= 300; end /= 300; wait /= 300;
        if (!g_profile) {   // (per-call timing off: frame rate and the frame's end only)
            printf("[Perf] %.1f fps: frame %.1f ms, present %.1f (GPU wait %.1f), 3D %s, %u code flushes\n", 1000.0 / frame_ms,
                   frame_ms, end, wait, osGet3DSliderState() > 0.01f ? "on" : "off", g_code_flushes);
            g_code_flushes = 0;
            LogMemory("every 300 frames");
            PollInput();
            if (!aptMainLoop()) S3ERuntime::Instance().RequestQuit();
            return;
        }
        printf("[Perf] %.1f fps: frame %.1f ms = game %.1f + runtime %.1f + record %.1f + present %.1f (GPU wait %.1f), 3D %s, %u code flushes\n",
               1000.0 / frame_ms, frame_ms, frame_ms - host, host - rec - end, rec, end, wait,
               osGet3DSliderState() > 0.01f ? "on" : "off", g_code_flushes);
        g_code_flushes = 0;
        {   // the imports that took the most time, per frame
            const std::vector<std::string>& names = S3ERuntime::Instance().Imports();
            std::vector<int> order;
            for (int i = 0; i < 512; ++i) if (g_svc_calls[i]) order.push_back(i);
            std::sort(order.begin(), order.end(), [](int a, int b) { return g_svc_ticks[a] > g_svc_ticks[b]; });
            unsigned total = 0;
            for (int i : order) total += g_svc_calls[i];
            printf("[Perf]  %.0f calls/frame; top:", total / 300.0);
            for (size_t k = 0; k < order.size() && k < 8; ++k) {
                const int i = order[k];
                printf(" %s %.2fms/%.0f", i < static_cast<int>(names.size()) ? names[i].c_str() : "callback",
                       g_svc_ticks[i] / CPU_TICKS_PER_MSEC / 300, g_svc_calls[i] / 300.0);
            }
            printf("\n");
            memset(g_svc_ticks, 0, sizeof(g_svc_ticks));
            memset(g_svc_calls, 0, sizeof(g_svc_calls));
        }
        LogMemory("every 300 frames");
    }
    PollInput();
    if (!aptMainLoop()) S3ERuntime::Instance().RequestQuit();   // closed from the HOME menu
}
}

namespace {
// First run (no game/ yet): installs from a *.n-gage in sdmc:/3ds/mgs_mobile/, whatever its name, trying them in
// name order until one is this game (another game's package fails the check before anything is written). The
// archives are unpacked by the game's own code on the native CPU (installer.cpp); no PNG textures. Progress on the
// bottom screen. Always ends the run (false): after setup the player starts the game again; otherwise the message
// (no package, or why it failed) stays up until START.
bool InstallFromPackage(const std::string& dir) {
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
        WaitForExit("\nNo game files yet.\n\nCopy your Metal Gear Solid Mobile package\n(the .n-gage file, any name) to\n" + dir +
                    "\nand start the game again.");
        return false;
    }
    std::string errors;
    for (const fs::path& p : packages) {
        const std::string name = p.filename().u8string();
        ConsoleWrite(("\x1b[2J\x1b[3;2HSetting up Metal Gear Solid Mobile\x1b[5;2Hfrom " + name.substr(0, 33)).c_str());
        std::string error;
        int last_pct = -1;
        const bool ok = Installer::Install(p, dir, [&](float f, const std::string& status) {
            const int pct = static_cast<int>(f * 100.0f);
            if (pct == last_pct && status.find('(') != std::string::npos) return;   // (per-file steps: on percent changes)
            last_pct = pct;
            char line[96];
            snprintf(line, sizeof line, "\x1b[8;2H%-38.38s\x1b[10;2H%3d%%", status.c_str(), pct);
            ConsoleWrite(line);
        }, &error);
        if (ok) {   // the game starts on the next launch, with all of the console's memory for itself
            WaitForExit("\nSetup complete.\n\nStart Metal Gear Solid Mobile again\nto play.");
            return false;
        }
        errors += name + ": " + error + "\n";
    }
    WaitForExit("\nSetup failed.\n\n" + errors);
    return false;
}
}

// cpu_native.cpp, before it reports a crash in our own code and stops: the message on the bottom screen.
extern "C" void ShowLogOnScreen() {
    g_console_hidden = false;
    g_console_log = true;
}

int main(int argc, char** argv) {
    gfxInitDefault();
    gfxSet3D(!g_small_memory);   // stereoscopic top screen (gl3ds.cpp draws both eyes when the slider is up)
    GL3DS::SetStereo(!g_small_memory);   // (Old 3DS / 2DS: one eye, no right-eye target)
    StartLog();
    std::set_new_handler(OutOfMemory);
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    if (is_new) osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
    std::cout << "MGS Mobile - 3DS port (" << (is_new ? "New 3DS" : "3DS") << "), application memory free "
              << osGetMemRegionFree(MEMREGION_APPLICATION) / 1024 << " KB" << std::endl;
    LoadEnvFile();
    for (int i = 1; i < argc; ++i) {   // NAME=VALUE arguments (3dslink: test runs without editing mgs_env.txt)
        const char* eq = strchr(argv[i], '=');
        if (eq && eq != argv[i]) {
            setenv(std::string(argv[i], eq - argv[i]).c_str(), eq + 1, 1);
            std::cout << "[Env] " << argv[i] << std::endl;
        }
    }
    g_profile = getenv("MGS_PROFILE") != nullptr;   // per-syscall timing in the [Perf] lines (slows the game down)
    g_console_log = getenv("MGS_VERBOSE") != nullptr;
    g_perf_log = g_console_log || g_profile || getenv("MGS_PERF");   // MGS_PERF=1: the [Perf] lines only
    LogLastCrashDump();

    const std::string dir = Platform::U8String(Platform::ExeDir());
    Settings& settings = Settings::Instance();
    settings.Load();
    // Verbose log (every file opened, first use of each import, ...) only on request: printing it to the bottom
    // screen and the SD card slowed loading down. MGS_VERBOSE=1 in mgs_env.txt (README.md section 7).
    settings.debug_mode = getenv("MGS_VERBOSE") != nullptr;

    if (!Installer::Installed(dir) && !InstallFromPackage(dir)) {
        gfxExit();
        return 0;
    }
    VirtualFileSystem::Instance().SetAssetRoot(dir + "game/data");
    // The data files in one pack (game/data.pak, built on the first start: an SD-card open costs 15-40 ms on an
    // Old 3DS, and a room loads dozens of files). MGS_NO_PACK=1: the files one by one.
    const std::string pack = getenv("MGS_NO_PACK") ? "" : dir + "game/data.pak";
    bool built = false;
    auto building = [&built] {
        built = true;
        ConsoleWrite("\x1b[2J\x1b[3;2HPreparing the game data\x1b[5;2H(first start only, about a minute)");
    };
    if (!g_small_memory && !getenv("MGS_NO_PRELOAD")) {   // room changes then don't wait for the SD card (Old 3DS / 2DS: no room for it)
        std::cout << "Loading the game data into memory..." << std::endl;
        VirtualFileSystem::Instance().PreloadAll(true, pack, building);
    } else {
        VirtualFileSystem::Instance().PreloadAll(false, pack, building);   // the index: lookups without the card
        // (the C heap has ~4 MB to spare: gameplay peaks at 3.3 MB of 7)
        VirtualFileSystem::Instance().SetRecentCacheBytes(1536 * 1024);
    }
    if (built) ConsoleWrite("\x1b[2J");   // (the bottom screen blank again)
    TexturePack::Init(Platform::ExeDir() / "textures");   // custom textures, if a textures folder was copied too

    S3EBinary binary;
    if (!LoadS3E(dir + "game/mgs_ngi", binary)) {
        WaitForExit("Could not load game/mgs_ngi.");
        gfxExit();
        return 1;
    }

    // Guest heap: 64 MB where it fits (gameplay peaks at 34 MB), less on an Old 3DS: the application memory
    // left after libctru's heaps, minus the image, stack and framebuffer (~5 MB).
    if (!getenv("MGS_HEAP_MB")) {
        const s64 free_mb = static_cast<s64>(osGetMemRegionFree(MEMREGION_APPLICATION)) / (1024 * 1024);
        const s64 window_mb = static_cast<s64>(OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN - envGetHeapSize()) / (1024 * 1024);   // address space left
        const s64 heap_mb = std::min<s64>({64, free_mb - 5, window_mb - 5});
        std::cout << "[3DS] Heap " << envGetHeapSize() / (1024 * 1024) << " MB, linear " << envGetLinearHeapSize() / (1024 * 1024)
                  << " MB, free " << free_mb << " MB: guest heap " << heap_mb << " MB" << std::endl;
        // Gameplay peaks at 34 MB of guest heap. An original 3DS / 2DS leaves ~5 MB under the Homebrew Launcher
        // (README.md section 6.28), so say what's wrong instead of failing to map it. (MGS_HEAP_MB skips this.)
        if (heap_mb < 34) {
            WaitForExit("\nNot enough memory.\n\nMetal Gear Solid Mobile needs a\nNew Nintendo 3DS or New 2DS.\n\n"
                        "This console has " + std::to_string(std::max<s64>(0, heap_mb)) + " MB for the game,\nit needs 34 MB.");
            gfxExit();
            return 1;
        }
        setenv("MGS_HEAP_MB", std::to_string(heap_mb).c_str(), 1);
    }

    std::string cpu_error;
    std::unique_ptr<Cpu> cpu = Cpu::CreateNative(&cpu_error);
    if (!cpu || !SetupSandbox(cpu.get(), binary, ImageBase())) {
        WaitForExit("Could not set up the game's memory. " + cpu_error);
        gfxExit();
        return 1;
    }
    g_cpu = cpu.get();

    S3ERuntime& rt = S3ERuntime::Instance();
    rt.Init(g_cpu, binary.imports);
    rt.SetIcf(binary.config);
    rt.SetHostClientSize(400, 240);
    ApplySettings();
    rt.SetFrameCallback([](const uint16_t* pixels, int w, int h) { GLES::Instance().PresentSoftware(pixels, w, h); });
    GLES::Instance().SetFrameCallback([](const uint32_t*, int, int) {});
    GLES::Host host;
    host.make_current = [] { return GL3DS::Init(); };
    host.get_proc = [](const char* name) { return GL3DS::Proc(name); };
    host.window_size = [](int& w, int& h) { w = 400; h = 240; };
    host.swap = [] { GL3DS::EndFrame(); FrameTick(); };
    GLES::SetHost(std::move(host));
    rt.SetExitCallback([] {});
    // F1 overlay (bottom screen): drawn after each game frame while open, which pauses the game until it closes.
    GLES::Instance().SetOverlayCallback([](int, int) {
        if (!Overlay::Instance().IsOpen()) return;
        GL3DS::BeginBottom();
        Overlay::Instance().Render(320, 240);
        GL3DS::EndBottom();
    });
    GLES::Instance().SetPauseHooks(
        [] {
            S3ERuntime& r = S3ERuntime::Instance();
            return ((Overlay::Instance().IsOpen() && !r.LuaPending()) || r.FocusPaused()) && !r.IsQuitRequested();
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
    const int err = g_cpu->Start(entry, 0xDEADBEEF);
    if (err != Cpu::OK && !rt.IsQuitRequested()) {
        uint32_t pc = 0, lr = 0;
        g_cpu->RegRead(Cpu::REG_PC, &pc);
        g_cpu->RegRead(Cpu::REG_LR, &lr);
        char where[64];
        snprintf(where, sizeof where, " at PC=0x%x, LR=0x%x", static_cast<unsigned>(pc), static_cast<unsigned>(lr));
        rt.DumpExitDiagnostics(true);   // [Hist] import counts, [Recent] the last syscalls (log only)
        WaitForExit("[EmuThread] Emulation stopped: " + std::string(g_cpu->ErrorText(err)) + where +
                    "\n\nThe log is in sdmc:/3ds/mgs_mobile/mgs_mobile.log.");
    } else {
        std::cout << "[EmuThread] Emulation stopped cleanly." << std::endl;
    }
    AudioOut::Instance().Stop();
    settings.Save();
    rt.DumpExitDiagnostics();
    GL3DS::Shutdown();   // (the render thread finishes its frame)
    if (g_link_sock >= 0) close(g_link_sock);
    socExit();
    gfxExit();
    return 0;
}
