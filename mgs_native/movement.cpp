#include "movement.h"
#include "loader.h"
#include "cpu.h"
#include "luachunk.h"
#include "runtime.h"
#include "settings.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

// The game's movement direction (README.md §6.22), once per frame in the input object's update 0x4A06CD00:
//  - 0x4A06CA48 turns the held direction keys into a 3x3 grid index (4 = none): up 7, down 1, then left +1 /
//    right -1. Each "key" is tested through 0x4A06CC7C(obj, dir, rot), which rotates it by the screen
//    rotation rot (r6, 90 degree steps). Left / right only add to up / down when the byte at
//    [0x4A07A44C()]+0x5E is set; it is cleared in the constructor (0x4A08DE70) and never set: diagonals were
//    built in and left off. Without it the vertical key wins.
//  - The index picks an angle from the table at 0x4A0DE9FC (4096 = a full turn: down 0, left 1024, up 2048,
//    right 3072, the diagonals between), loaded into r7 at 0x4A06CDC6; cos / sin (0x4A04C5CC / 0x4A04C628)
//    give the unit vector (-sin, 0, cos) stored at obj+8 (x, y, z; 1.0 = 4096) at 0x4A06CDF4. Snake turns
//    to it and runs that way (heading 0x4A0A785E). His speed doesn't follow the vector's length (the run
//    animation moves him), so analog movement sets the direction only.
namespace {

constexpr float kDeadZone = 0.30f, kRelease = 0.22f;   // stick: moves past 30%, stops below 22%

struct Stick { std::atomic<float> x{0}, y{0}; };
Stick g_stick[Movement::SOURCE_COUNT];
std::atomic<int> g_last{-1};   // the source moved last
uint32_t g_rot = 0;            // emu thread: the screen rotation of the current update

bool Active(const Stick& s) { return s.x * s.x + s.y * s.y > kRelease * kRelease; }

// The stick in use: the one moved last while it's out of its dead zone, else any other that is.
const Stick* Current() {
    const int last = g_last;
    if (last >= 0 && Active(g_stick[last])) return &g_stick[last];
    for (const Stick& s : g_stick) if (Active(s)) return &s;
    return nullptr;
}

bool Trace() { static const bool t = getenv("MGS_TRACE_MOVE") != nullptr; return t; }

} // namespace

namespace Movement {

void SetStick(Source s, float x, float y) {
    Stick& st = g_stick[s];
    const bool was = Active(st);
    st.x = x; st.y = y;
    if (Active(st) && !was) g_last = s;
}

void StickKeys(float x, float y, bool* held, bool& up, bool& down, bool& left, bool& right) {
    up = down = left = right = false;
    const float m = std::sqrt(x * x + y * y);
    *held = *held ? m > kRelease : m > kDeadZone;
    if (!*held) return;
    const float pi = 3.14159265f;
    const float a = std::atan2(y, x);   // 0 = right, pi/2 = down
    const int sectors = Settings::Instance().diagonal_movement ? 8 : 4;
    const int s = static_cast<int>(std::floor(a / (2 * pi / sectors) + 0.5f)) & (sectors - 1);
    if (sectors == 4) { right = s == 0; down = s == 1; left = s == 2; up = s == 3; return; }
    right = s == 7 || s == 0 || s == 1; down = s >= 1 && s <= 3; left = s >= 3 && s <= 5; up = s >= 5 && s <= 7;
}

void InstallHooks(Cpu* uc) {
    if (Trace()) {   // debug: Snake's position (Lua ObjectGetPosition 0x4A0393A6; its table comes from this pointer)
        uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
            uint32_t p = 0;
            int32_t xyz[3] = {};
            uc->RegRead(Cpu::REG_R0, &p);
            if (uc->MemRead(p, xyz, sizeof(xyz)) == Cpu::OK)
                std::cout << "[Move] Snake at " << xyz[0] << " " << xyz[1] << " " << xyz[2] << std::endl;
        }, nullptr, G(0x4A0393B6), G(0x4A0393B6));
    }

    // Diagonals: the flag test in 0x4A06CA48 (`ldrb r0, [r0, #0x1e]` then `cmp r0, #0` here). r6 = rot.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
        static int frames = 0;
        if (Trace() && ++frames % 15 == 0 && !S3ERuntime::Instance().LuaPending())
            S3ERuntime::Instance().RunLua(LuaChunk().GuardSnakeAlive().Call("ObjectGetPosition", {"Snake"}));
        uc->RegRead(Cpu::REG_R6, &g_rot);
        if (!Settings::Instance().diagonal_movement) return;
        const uint32_t one = 1;
        uc->RegWrite(Cpu::REG_R0, &one);
    }, nullptr, G(0x4A06CA92), G(0x4A06CA92));

    // Analog direction: r7 = the table angle for the keys' index (r6), about to become the vector. A stick
    // gives its own angle, rotated like the keys are. Only while the keys move Snake (the index-4 "no
    // direction" path doesn't come here), so the keys still decide when he moves.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
        if (!Settings::Instance().analog_movement) return;
        const Stick* s = Current();
        if (!s) return;
        const float pi = 3.14159265f;
        const float a = std::atan2(-s->x.load(), s->y.load()) / (2 * pi) * 4096.0f;   // down 0, left 1024, up 2048
        const uint32_t angle = (static_cast<uint32_t>(std::lround(a) + 4096 + 1024 * (g_rot & 3))) & 4095;
        if (Trace()) {
            uint32_t table = 0, idx = 0;
            uc->RegRead(Cpu::REG_R7, &table); uc->RegRead(Cpu::REG_R6, &idx);
            std::cout << "[Move] index " << idx << " table angle " << table << " -> stick " << angle << " (rot " << g_rot << ")" << std::endl;
        }
        uc->RegWrite(Cpu::REG_R7, &angle);
    }, nullptr, G(0x4A06CDC8), G(0x4A06CDC8));
}

} // namespace Movement
