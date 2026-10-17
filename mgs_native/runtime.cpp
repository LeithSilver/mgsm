#include "runtime.h"
#include "loader.h"
#include "audio.h"
#include <algorithm>
#include "vfs.h"
#include "gles.h"
#include "settings.h"
#include "story.h"
#include "photomode.h"
#include "movement.h"
#include "custommodel.h"
#include <memory>
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <set>
#include <cstdlib>

static std::vector<uint32_t> g_hist;   // calls per import index (an array: this runs on every syscall)

static std::string ReadGuestString(Cpu* uc, uint32_t addr, size_t max_len = 256) {
    if (addr == 0) return "";
    std::vector<char> buf(max_len, 0);
    uc->MemRead(addr, buf.data(), max_len - 1);
    return std::string(buf.data());
}

S3ERuntime& S3ERuntime::Instance() {
    static S3ERuntime instance;
    return instance;
}

// Debug: MGS_WATCH=<hex addr> logs every guest write to that 4-byte word, with PC.
// MGS_DUMP_ONLY=1 limits [Watch]/[Break] output to the GL dump frame (F12), so it lines up with [GLDump].
static bool OutsideDump() {
    static const bool only = getenv("MGS_DUMP_ONLY") != nullptr;
    return only && !GLES::Instance().IsDumping();
}

static void WatchHook(Cpu* uc, uint64_t addr, int size, int64_t value, void*) {
    if (OutsideDump()) return;
    uint32_t pc = 0, lr = 0;
    uc->RegRead(Cpu::REG_PC, &pc);
    uc->RegRead(Cpu::REG_LR, &lr);
    std::cout << "[Watch] write" << size << " @0x" << std::hex << addr << " = 0x" << static_cast<uint32_t>(value)
              << " pc=0x" << pc << " lr=0x" << lr << std::dec << std::endl;
}

// Debug: MGS_BREAK=<hex addr> prints registers on 8 hits of that instruction (after MGS_BREAK_SKIP hits).
static int g_break_hits = 0;
static void BreakHook(Cpu* uc, uint64_t addr, uint32_t, void*) {
    int& hits = g_break_hits;
    static const int skip = getenv("MGS_BREAK_SKIP") ? atoi(getenv("MGS_BREAK_SKIP")) : 0;
    static const int regs[] = {Cpu::REG_R0, Cpu::REG_R1, Cpu::REG_R2, Cpu::REG_R3, Cpu::REG_R4,
                               Cpu::REG_R5, Cpu::REG_R6, Cpu::REG_R7, Cpu::REG_SP, Cpu::REG_LR};
    // MGS_BREAK_IF=rN=hex: only count hits where that register has that value
    static int cond_reg = -1; static uint32_t cond_val = 0; static bool cond_init = false;
    if (!cond_init) {
        cond_init = true;
        if (const char* c = getenv("MGS_BREAK_IF")) { cond_reg = atoi(c + 1); const char* eq = strchr(c, '='); if (eq) cond_val = strtoul(eq + 1, nullptr, 16); }
    }
    if (cond_reg >= 0 && cond_reg < 8) { uint32_t v = 0; uc->RegRead(regs[cond_reg], &v); if (v != cond_val) return; }
    if (OutsideDump()) return;
    ++hits;
    static const int maxh = getenv("MGS_BREAK_MAX") ? atoi(getenv("MGS_BREAK_MAX")) : 8;
    if (hits <= skip || hits > skip + maxh) return;
    std::cout << "[Break] #" << hits << " 0x" << std::hex << addr;
    for (int i = 0; i < 10; ++i) { uint32_t v = 0; uc->RegRead(regs[i], &v); std::cout << (i < 8 ? " r" + std::to_string(i) : (i == 8 ? " sp" : " lr")) << "=0x" << v; }
    uint32_t sp = 0, words[128] = {};
    uc->RegRead(Cpu::REG_SP, &sp);
    uc->MemRead(sp, words, sizeof(words));
    std::cout << " bt:";
    for (uint32_t w : words) if ((w & 1) && w >= G(0x4A000000) && w < G(0x4A0F2000)) std::cout << " " << w;
    std::cout << std::dec << std::endl;
}

void S3ERuntime::Init(Cpu* uc, const std::vector<std::string>& imports) {
    uc_ = uc;
    imports_ = imports;
    heap_.Init(HEAP_BASE, HEAP_SIZE);
    start_time_ = std::chrono::steady_clock::now();
    quit_requested_ = false;
    frame_count_ = 0;

    VirtualFileSystem::Instance().SetEngine(uc);
    GLES::Instance().SetGuestAlloc([this](uint32_t size) { return GuestAlloc(size); });

    // HLE: dzArchiveAttach (0x4A0D3CF8, Thumb) -> "movs r0,#0; bx lr" (success).
    // The device mounts all.dz through the game's own derbh code; we serve the same files already
    // extracted, so attaching is a no-op. It must still succeed: CResourceStack (0x4A081A0C) then sets
    // its "archive mounted" flag (+0x1C), which CEnvironmentInst::Serialise (0x4A0B8EB0) uses to skip a
    // per-object hash the shipped level data does not contain.
    const uint16_t dz_attach_stub[2] = { 0x2000, 0x4770 };
    uc->MemWrite(G(0x4A0D3CF8), dz_attach_stub, sizeof(dz_attach_stub));

    // Fix: skinned-mesh transform mode leak (Snake drawn invisible / giant and upside down).
    // The software-skinning block callback (0x4A044BF8, ARM) ends at 0x4A045734 with:
    //   if ((model->flags & 0x80000) && (ctx->flags & 0x1000 /*GL raster*/)) set vert stream only (keep mode)
    //   else table[+8](verts, n)  -> IwGx vertex mode 1 = world space (MV = camera matrix ctx+0x140)
    // Its output is world space, so keeping the mode is only right when mode 1 is already current. When a
    // camera-facing billboard (0x4A097964, mode 2 = rotation-free) was submitted just before - Snake's own
    // sprite is - his mesh inherits mode 2 and is drawn as if his world position were a view-space offset.
    // Turning the "beq" at 0x4A045740 into "b" always takes the mode-1 path; identical when mode 1 was current.
    const uint32_t skin_always_world = 0xEA000007;
    uc->MemWrite(G(0x4A045740), &skin_always_world, 4);

    // Full version unlock.
    // License state lives at 0x4A0F1C58: byte +0 = NGI platform present, byte +1 = FULL VERSION UNLOCKED
    // (set to 1 when the NGI license check passes and the encrypted assets are mapped).
    // IsFullVersion() = 0x4A081CEC (refreshes via 0x4A081A8C, returns byte +1); IsNGIDemoVersion = 1 ^ it.
    // Its callers: room exit 0x4A06D7CC (dest room in the full-only table 0x4A06DC7C && !full -> trial-end
    // sequence at 0x4A06D7FA), trial-room helper 0x4A06E388, DRM pages 0x4A074596 / 0x4A074712, level
    // transition 0x4A0939CA, 0x4A09486C, and 0x4A0967F2 (full -> menu page, else UpsellScreen).
    // So the fix is to report "full" (byte +1 = 1) from both license setters:
    //   0x4A081D00 (encrypted asset mount):  ldr r0,=0x4A0F1C58; movs r1,#1; strb r1,[r0,#1]; movs r0,#0; bx lr
    //   0x4A081A8C (license refresh):        same, with its own literal
    // Every other trial check reads this flag, so nothing else needs patching. (Lua IsDemoVersion,
    // 0x4A02FFEC, reads s3eConfigGetInt "EmbedDemo", which we don't define -> its default 0 = false.)
    // An earlier version also wrote 0 (= trial) here and patched 0x4A06D7EE to branch into the trial-end
    // sequence, so the trial ended on leaving the first room. Do not patch 0x4A06D7EE.
    const uint16_t stub_enc_assets[5] = { 0x4816, 0x2101, 0x7041, 0x2000, 0x4770 };
    uc->MemWrite(G(0x4A081D00), stub_enc_assets, sizeof(stub_enc_assets));

    const uint16_t stub_license[5] = { 0x4811, 0x2101, 0x7041, 0x2000, 0x4770 };
    uc->MemWrite(G(0x4A081A8C), stub_license, sizeof(stub_license));

    uc->AddSvcHook(IntrHook, this);

    // Front-end page change (0x4A077460(pageManager, page, flag), reached via 0x4A077486 e.g. title ->
    // "MgsMenuPage"): keys held across it are swallowed until released. The title advances on OK's press
    // but the menu acts on OK's release, so holding OK through "press any key" would otherwise pick a menu item.
    // Widescreen 3D view. The game builds its 3D view rect + projection in 0x4A0BC55C, capping the view's
    // aspect at the setting [0x4A0F1BB0]+0x6C (4.12 fixed, 1.25 = 300x240) and centring a narrower box on
    // wider screens. Raising the cap to the screen aspect right before that function runs makes the view fill
    // the screen; the focal length (max(w,h) / cap) stays = the short side, i.e. Hor+.
    // --pillarbox keeps the original cap.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        auto* rt = static_cast<S3ERuntime*>(self);
        uint32_t params = 0;   // settings object (CParamManager), as 0x4A07A44C returns it: [0x4A0F1BAC + 4]
        uc->MemRead(G(0x4A0F1BAC) + 4, &params, 4);
        if (!params) return;
        uint32_t cap = 0;
        uc->MemRead(params + 0x6C, &cap, 4);
        if (!rt->view_aspect_orig_) rt->view_aspect_orig_ = cap;
        // Full-bleed: the view spans the whole long side (the phone kept a 10 px margin each side).
        const int lng = (std::max)(rt->surf_w_, rt->surf_h_), shr = (std::max)(1, (std::min)(rt->surf_w_, rt->surf_h_));
        uint32_t want = rt->view_aspect_orig_;
        if (!rt->pillarbox_) want = (std::max)(want, static_cast<uint32_t>((static_cast<uint64_t>(lng) << 12) / shr + 1));
        if (want != cap) uc->MemWrite(params + 0x6C, &want, 4);
        // FOV: the focal length is (max(w,h) / cap) * [params+0x70] (4.12); scale that factor by 100/fov%.
        uint32_t focal = 0;
        uc->MemRead(params + 0x70, &focal, 4);
        if (!rt->view_focal_orig_) rt->view_focal_orig_ = focal;
        int fov = rt->fov_percent_.load();
        if (PhotoMode::Instance().Active()) fov = static_cast<int>(fov * PhotoMode::Instance().Fov() / 100.0f);   // photo zoom
        fov = (std::max)(10, fov);
        const uint32_t want_focal = static_cast<uint32_t>(static_cast<uint64_t>(rt->view_focal_orig_) * 100 / fov);
        if (want_focal != focal) uc->MemWrite(params + 0x70, &want_focal, 4);
        static const bool trace = getenv("MGS_TRACE_VIEW") != nullptr;
        if (trace) {
            uint32_t r0 = 0; uc->RegRead(Cpu::REG_R0, &r0);
            std::cout << "[View] 0x4A0BC55C this=0x" << std::hex << r0 << " params=0x" << params << " cap 0x" << cap
                      << " -> 0x" << want << std::dec << " surf " << rt->surf_w_ << "x" << rt->surf_h_ << std::endl;
        }
    }, this, G(0x4A0BC55C), G(0x4A0BC55C));

    // Cinematic letterbox (0x4A0895E4(effect, rect): rect.y += bar, rect.h -= 2*bar, bar = [effect+0x38]) is
    // sized for the phone's 300-wide view. On a view wider than the cinematic aspect the bar goes negative and
    // the rect grows past the screen (IwGxSetViewRect "extends beyond screen", every frame): no bars needed.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
        uint32_t effect = 0;
        int32_t bar = 0;
        uc->RegRead(Cpu::REG_R0, &effect);
        if (uc->MemRead(effect + 0x38, &bar, 4) == Cpu::OK && bar < 0) { bar = 0; uc->MemWrite(effect + 0x38, &bar, 4); }
    }, this, G(0x4A0895E4), G(0x4A0895E4));

    // Title illustration (Snake, material hash 0xCBF63179, texture common/frontend/cbf63179). The front end
    // lays it out at x = -4 - w/4 (left edge; -84 on the phone's 320 px), so a wider screen pushes it off the
    // left side (-104 at 400 px, gone at 640). On screens wider than the phone its left edge is kept at -10 or
    // more. The image draw 0x4A07FB94(ctx, material, ...) takes the position from ctx +0 (int16 x, y) and clips
    // to ctx +4 ({int16 x, y, w, h}, relative to that position): the element's own draw context, built per
    // element by 0x4A07F510 and dropped after the draw (0x4A0800D4 reads only +0x10). Moving x right by d moves
    // the clip's x left by d and widens it by d (same left edge on screen, room for the moved picture), so this
    // one draw moves; the layout stays as the game made it.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        auto* rt = static_cast<S3ERuntime*>(self);
        if (rt->surf_w_ <= 320 || rt->surf_w_ <= rt->surf_h_) return;   // (portrait, or the phone's own width)
        uint32_t ctx = 0, material = 0, hash = 0;
        uc->RegRead(Cpu::REG_R1, &material);
        if (!material || uc->MemRead(material + 4, &hash, 4) != Cpu::OK || hash != 0xCBF63179) return;
        uc->RegRead(Cpu::REG_R0, &ctx);
        int16_t c[6] = {};   // x, y, clip x, y, w, h
        if (uc->MemRead(ctx, c, sizeof(c)) != Cpu::OK || c[0] >= -10) return;
        const int16_t d = static_cast<int16_t>(-10 - c[0]);
        c[0] = -10;
        c[2] = static_cast<int16_t>(c[2] - d);
        c[4] = static_cast<int16_t>(c[4] + d);
        uc->MemWrite(ctx, c, sizeof(c));
    }, this, G(0x4A07FB94), G(0x4A07FB94));

    // Soft-key bars (CIwGuiSoftkeys; setup 0x4A08D4B4(this) finds its children: +0x78/+0x7C = left/right
    // label, +0x80/+0x84 = left/right frame). Recorded so the "hide soft keys" option can clear their
    // visible flag (element +0xC, bit 0x8) every frame; the game re-shows them whenever a label changes.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        uint32_t obj = 0;
        uc->RegRead(Cpu::REG_R0, &obj);
        auto* rt = static_cast<S3ERuntime*>(self);
        if (std::find(rt->softkey_bars_.begin(), rt->softkey_bars_.end(), obj) == rt->softkey_bars_.end()) {
            rt->softkey_bars_.push_back(obj);
            if (getenv("MGS_TRACE_VIEW")) std::cout << "[SoftKeys] bar 0x" << std::hex << obj << std::dec << std::endl;
        }
    }, this, G(0x4A08D4B4), G(0x4A08D4B4));

    // Level-geometry culling: CCameraPVS apply 0x4A05207C(pvs) marks geom i visible (geom+0x60 |= 1) only if
    // bit i of the camera position's precomputed set (pvs+0x10) is set; the test is `tst` + `beq` at 0x4A0520A0.
    // Those sets were baked for the phone's narrow view, so a wider view pops geometry in and out at its edges.
    // "Draw all level geometry" clears Z there so every geom of the room passes.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        if (!static_cast<S3ERuntime*>(self)->draw_all_geometry_ && !PhotoMode::Instance().Active()) return;
        uint32_t cpsr = 0;
        uc->RegRead(Cpu::REG_CPSR, &cpsr);
        cpsr &= ~(1u << 30);
        uc->RegWrite(Cpu::REG_CPSR, &cpsr);
    }, this, G(0x4A0520A0), G(0x4A0520A0));

    // Level select: inside our RoomChange call (see PumpRoomChange), replace lua_tolstring(L, 1) (the BL at
    // 0x4A088BB8) by r0 = our name and continue at 0x4A088BBC.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        auto* rt = static_cast<S3ERuntime*>(self);
        if (getenv("MGS_TRACE_ROOMS") && !rt->room_change_active_) std::cout << "[RoomChange] called by the game" << std::endl;
        if (!rt->room_change_active_) return;
        uint32_t pc = G(0x4A088BBC), cpsr = 0;
        uc->RegWrite(Cpu::REG_R0, &rt->room_name_buf_);
        uc->RegWrite(Cpu::REG_PC, &pc);
        uc->RegRead(Cpu::REG_CPSR, &cpsr);
        cpsr |= 0x20;   // stay in Thumb
        uc->RegWrite(Cpu::REG_CPSR, &cpsr);
    }, this, G(0x4A088BB8), G(0x4A088BB8));

    // lua_tolstring(L, ...) 0x4A02FCFA: remember a lua_State for level select
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        auto* rt = static_cast<S3ERuntime*>(self);
        if (!rt->lua_seen_) uc->RegRead(Cpu::REG_R0, &rt->lua_seen_);
    }, this, G(0x4A02FCFA), G(0x4A02FCFA));

    // Original debug commands (F1 > General, debug mode; README §6.16). The game's release build compiled these
    // Lua bindings down to stubs; the scripts still use them.
    //  InDebugMode() / IsFrameRateTest(): lua_pushboolean(L, r1) with r1 = 0 (movs at 0x4A028B52 / 0x4A0330C6);
    //  at the BL that follows, r1 = our setting.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        uint32_t v = static_cast<S3ERuntime*>(self)->lua_debug_mode_ ? 1 : 0;
        uc->RegWrite(Cpu::REG_R1, &v);
    }, this, G(0x4A028B54), G(0x4A028B54));
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        uint32_t v = static_cast<S3ERuntime*>(self)->frame_rate_test_ ? 1 : 0;
        uc->RegWrite(Cpu::REG_R1, &v);
    }, this, G(0x4A0330C8), G(0x4A0330C8));
    //  TraceMsg(msg) 0x4A0216F4 / AssertMsg(msg) 0x4A02345C: `movs r0, #1; bx lr` stubs. Print the string argument
    //  (L in r0; first argument = TValue at L->base (+12); TString data at +16).
    auto lua_msg = [](Cpu* uc, S3ERuntime* rt, const char* tag) {
        uint32_t L = 0, base = 0, tv[2] = {};
        uc->RegRead(Cpu::REG_R0, &L);
        if (uc->MemRead(L + 12, &base, 4) != Cpu::OK || uc->MemRead(base, tv, 8) != Cpu::OK || tv[1] != 4) return;
        const std::string msg = ReadGuestString(uc, tv[0] + 16, 256);
        std::cout << "[Lua] " << tag << ": " << msg << std::endl;
        if (rt->debug_message_cb_ && std::string(tag) == "Assert") rt->debug_message_cb_(std::string("[Lua] ") + msg);
    };
    static decltype(lua_msg) s_lua_msg = lua_msg;
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        s_lua_msg(uc, static_cast<S3ERuntime*>(self), "Trace");
    }, this, G(0x4A0216F4), G(0x4A0216F4));
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        s_lua_msg(uc, static_cast<S3ERuntime*>(self), "Assert");
    }, this, G(0x4A02345C), G(0x4A02345C));

    // Photo mode free camera: IwGxSetViewMatrix(const CIwMat*) (ARM 0x4A02D2F4, reached through the Thumb veneer
    // 0x4A02D2F0) copies 12 words (3x3 rotation, translation) to ctx+0x3C. When the main 3D view sets it
    // (0x4A0BC55C, return address 0x4A0BC6E5) and photo mode is on, hand it our matrix instead.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void* self) {
        auto* rt = static_cast<S3ERuntime*>(self);
        uint32_t lr = 0, src = 0;
        uc->RegRead(Cpu::REG_LR, &lr);
        if (lr != G(0x4A0BC6E5) || !PhotoMode::Instance().Active()) return;
        int32_t game[12], mine[12];
        uc->RegRead(Cpu::REG_R0, &src);
        if (uc->MemRead(src, game, sizeof(game)) != Cpu::OK) return;
        if (!PhotoMode::Instance().ViewMatrix(game, mine)) return;
        if (!rt->photo_matrix_) rt->photo_matrix_ = rt->GuestAlloc(sizeof(mine));
        uc->MemWrite(rt->photo_matrix_, mine, sizeof(mine));
        uc->RegWrite(Cpu::REG_R0, &rt->photo_matrix_);
    }, this, G(0x4A02D2F4), G(0x4A02D2F4));

    // Custom textures: the CIwTexture upload function (Thumb 0x4A0607B0, r0 = the texture, its hash at +4) is
    // the only caller of glTexImage2D that matters; gles.cpp swaps in a replacement there.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
        uint32_t tex = 0, hash = 0;
        uc->RegRead(Cpu::REG_R0, &tex);
        if (uc->MemRead(tex + 4, &hash, 4) == Cpu::OK) GLES::Instance().SetUploadTexture(hash);
    }, this, G(0x4A0607B0), G(0x4A0607B0));

    uc->AddCodeHook(+[](Cpu*, uint64_t, uint32_t, void* self) {
        static_cast<S3ERuntime*>(self)->SwallowHeldKeys();
    }, this, G(0x4A077460), G(0x4A077460));

    // Skip intro: the splash-screen state's update (0x4A0A38D4) counts its page timer [0x4A0F1608] down and, at
    // zero, posts the timeout event that runs CGameStateSplashScreens::Advance (0x4A0A398C: next logo page, or the
    // front end after the last). Zeroing the timer on entry shows each logo for one frame.
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
        if (!Settings::Instance().skip_intro) return;
        const uint32_t zero = 0;
        uc->MemWrite(G(0x4A0F1608), &zero, 4);
    }, this, G(0x4A0A38D4), G(0x4A0A38D4));
    // Custom player model: the CPU skinning callback (0x4A044BF8) has written Snake's skinned vertices (r4); pose the
    // custom model with the same bone matrices (custommodel.cpp, README §6.24).
    uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) { CustomModel::OnSkinned(uc); }, nullptr, G(0x4A045734), G(0x4A045734));

    // RE: MGS_DUMP_SKIN=<file> dumps the CPU skinning callback's bones, skin sets and output every 200th call.
    if (getenv("MGS_DUMP_SKIN")) uc->AddCodeHook(+[](Cpu* uc, uint64_t, uint32_t, void*) {
        static int calls = 0;
        if (++calls % 200) return;
        auto rd = [&](uint32_t a) { uint32_t v = 0; uc->MemRead(a, &v, 4); return v; };
        uint32_t sp = 0, out = 0;
        uc->RegRead(Cpu::REG_SP, &sp);
        uc->RegRead(Cpu::REG_R4, &out);
        const uint32_t count = rd(sp + 0x84), ctx = rd(G(0x4A0EDBAC)), skel = rd(ctx + 4), skin = rd(ctx + 8);
        FILE* f = fopen(getenv("MGS_DUMP_SKIN"), "w");
        if (!f) return;
        const uint32_t bones = rd(skel + 0x10), nb = rd(skel + 0x14);
        fprintf(f, "call %d skel %08x skin %08x bones %u out %u\n", calls, skel, skin, nb, count);
        for (uint32_t b = 0; b < nb; ++b) {
            const uint32_t bone = rd(bones + b * 4);
            int32_t m[12]; uc->MemRead(bone + 0x24, m, sizeof(m));
            uint8_t raw[0x24]; uc->MemRead(bone, raw, sizeof(raw));
            fprintf(f, "bone %u %08x", b, bone);
            for (int i = 0; i < 12; ++i) fprintf(f, " %d", m[i]);
            fprintf(f, " raw");
            for (int i = 0; i < 0x24; ++i) fprintf(f, " %02x", raw[i]);
            fprintf(f, "\n");
        }
        {
            const uint32_t bskel = rd(skin + 0x18), bb = rd(bskel + 0x10), bn = rd(bskel + 0x14);
            for (uint32_t b = 0; b < bn && b < 64; ++b) {
                const uint32_t bone = rd(bb + b * 4);
                uint8_t raw[0x54]; uc->MemRead(bone, raw, sizeof(raw));
                fprintf(f, "bind %u %08x", b, bone);
                for (int i = 0; i < 0x54; ++i) fprintf(f, " %02x", raw[i]);
                fprintf(f, "\n");
            }
        }
        const uint32_t sets = rd(skin + 0x1C), ns = rd(skin + 0x20);
        for (uint32_t si = 0; si < ns; ++si) {
            const uint32_t set = rd(sets + si * 4), nv = rd(set + 0xC), ids = rd(set + 0x14);
            uint8_t bl[4]; uc->MemRead(ids, bl, 4);
            int n = bl[1] == 0xFF ? 1 : bl[2] == 0xFF ? 2 : bl[3] == 0xFF ? 3 : 4;
            uint32_t p = ids + (n == 1 ? 2 : 4);
            const int stride = n == 1 ? 10 : n == 2 ? 16 : n == 3 ? 24 : 30;
            fprintf(f, "set %u bones %d", si, n);
            for (int i = 0; i < n; ++i) fprintf(f, " %u", bl[i]);
            fprintf(f, " verts %u\n", nv);
            for (uint32_t v = 0; v < nv; ++v, p += stride) {
                uint8_t d[30]; uc->MemRead(p, d, stride);
                fprintf(f, "v %u", (d[0] | d[1] << 8) & 0x3FFF);
                for (int i = 2; i < stride; i += 2) fprintf(f, " %d", static_cast<int16_t>(d[i] | d[i + 1] << 8));
                fprintf(f, "\n");
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            int16_t q[3]; uc->MemRead(out + i * 6, q, 6);
            fprintf(f, "o %u %d %d %d\n", i, q[0], q[1], q[2]);
        }
        fclose(f);
    }, nullptr, G(0x4A045734), G(0x4A045734));
    Movement::InstallHooks(uc);   // 8-way and analog movement (movement.cpp)
    if (const char* b = getenv("MGS_BREAK")) {
        uint64_t a = strtoull(b, nullptr, 16);
        uc->AddCodeHook(BreakHook, nullptr, a, a);
    }
    if (const char* w = getenv("MGS_WATCH")) {
        uint64_t a = strtoull(w, nullptr, 16);
        uc->AddWriteHook(WatchHook, nullptr, a, a + 3);
    }

    uint32_t n = static_cast<uint32_t>(imports_.size());
    uc->WriteTrampoline(TRAMPOLINE_BASE + n * 8, n);   // the guest-callback return stub
}

// Blocks are handed out zeroed: the original bump allocator always returned fresh (zero) memory and
// the game may depend on that.
void S3ERuntime::ZeroGuest(Cpu* uc, uint32_t addr, uint32_t len) {
    static const std::vector<uint8_t> zeros(64 * 1024, 0);
    while (len) {
        uint32_t n = (std::min)(len, static_cast<uint32_t>(zeros.size()));
        uc->MemWrite(addr, zeros.data(), n);
        addr += n; len -= n;
    }
}

uint32_t S3ERuntime::GuestAlloc(uint32_t size) {
    uint32_t p = heap_.Alloc(size);
    if (!p) {
        std::cerr << "[Heap] Out of guest memory allocating " << size << " bytes (in use " << heap_.BytesInUse() << ")" << std::endl;
        return 0;
    }
    ZeroGuest(uc_, p, heap_.SizeOf(p));
    return p;
}

bool S3ERuntime::UseGL() {
    // Fixed for the run: IwGx picks its renderer once at startup (0x4A02F4FC).
    static const bool gl = getenv("MGS_SOFTWARE_GX") == nullptr && !Settings::Instance().software_renderer;
    return gl;
}

// Pick the guest screen orientation and render resolution from the host window. The phone version supports
// both orientations (Options > Portrait / Landscape); we present the window's shape as the device's native
// screen (no rotation: blit directions stay 0) and tell IwGx and the game through their screen-change
// callbacks, as the phone's OS does when it rotates.
void S3ERuntime::ApplyDisplay(Cpu* uc, bool notify) {
    const int cw = (std::max)(1, host_w_.load()), ch = (std::max)(1, host_h_.load());
    const bool landscape = cw > ch;
    // Guest screen: 240 on the short side, the long side following the window's aspect (even, 320..640), so
    // menus/codec fill any window shape (the game lays them out from the reported size). Near-square windows
    // keep 320x240 / 240x320 and get bars. --qvga (fixed_qvga_) keeps the phone's sizes; MGS_SURFACE=WxH forces one.
    int sw = landscape ? 320 : 240, sh = landscape ? 240 : 320;
    if (!fixed_qvga_) {
        const double aspect = landscape ? double(cw) / ch : double(ch) / cw;
        const int lng = (std::min)(640, (std::max)(320, static_cast<int>(240 * aspect / 2 + 0.5) * 2));
        if (landscape) sw = lng; else sh = lng;
    }
    static int ovr_w = 0, ovr_h = 0;
    static const bool ovr = [] { const char* s = getenv("MGS_SURFACE"); return s && sscanf(s, "%dx%d", &ovr_w, &ovr_h) == 2; }();
    if (ovr) { sw = landscape == (ovr_w > ovr_h) ? ovr_w : ovr_h; sh = landscape == (ovr_w > ovr_h) ? ovr_h : ovr_w; }
    double scale = (std::min)(double(cw) / sw, double(ch) / sh);
    scale = (std::max)(1.0, (std::min)(scale, 4096.0 / (std::max)(sw, sh)));
    const int fw = static_cast<int>(sw * scale + 0.5), fh = static_cast<int>(sh * scale + 0.5);
    // The output is the window (up to 4096): the game in a centred box, the overlay and touch controls to its edges
    GLES::Instance().SetDisplay(sw, sh, fw, fh, (std::min)(cw, 4096), (std::min)(ch, 4096));

    const bool changed = display_init_ && (sw != surf_w_ || sh != surf_h_);   // rotation or new aspect
    if (!display_init_) std::cout << "[Display] Window " << cw << "x" << ch << " -> screen " << sw << "x" << sh << ", output " << fw << "x" << fh << std::endl;
    surf_w_ = sw; surf_h_ = sh;
    display_init_ = true;
    if (!changed || !notify) return;

    std::cout << "[Display] Screen is now " << sw << "x" << sh << (landscape ? " (landscape)" : " (portrait)") << std::endl;
    if (!orient_struct_) orient_struct_ = GuestAlloc(20);
    const uint32_t orient[5] = { 1u, uint32_t(sw), uint32_t(sh), 0u, uint32_t(sw * 2) }; // changed, w, h, blit dir, pitch
    uc->MemWrite(orient_struct_, orient, sizeof(orient));
    for (auto& cb : surface_cbs_) pending_cbs_.push_back({cb.first, orient_struct_, cb.second});
}

void S3ERuntime::SetFrameCallback(std::function<void(const uint16_t*, int, int)> cb) {
    frame_callback_ = cb;
}

// Make the game recompute its 3D view rect + projection (0x4A0BC55C) on its next frame: set the view
// object's "dirty" byte (view = [0x4A0F232C], byte +0x4C). Emu thread.
void S3ERuntime::RefreshView() {
    if (!uc_) return;
    uint32_t view = 0;
    uc_->MemRead(G(0x4A0F232C), &view, 4);
    if (!view) return;
    const uint8_t one = 1;
    uc_->MemWrite(view + 0x4C, &one, 1);
}

// Game clock (ms since start) excluding time spent paused in the F1 overlay, so the game sees no time jump.
int64_t S3ERuntime::GameMs() const {
    if (frozen_) return frozen_ms_;   // photo mode
    return RunningMs();
}

int64_t S3ERuntime::RunningMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count() - paused_ms_;
}

// Photo mode, once per frame (emu thread): moves the free camera and freezes / thaws the game clock. While frozen
// GameMs stands still, so the game runs no updates but keeps rendering; each StepFrame (F6) moves the frozen
// clock on by one update at MaxUPS 30. Thawing counts the frozen time as paused, so the clock resumes where it stopped.
void S3ERuntime::UpdatePhotoMode() {
    PhotoMode& pm = PhotoMode::Instance();
    pm.Update();
    const bool want = pm.Active();
    GLES::Instance().SetHide2D(want && pm.HudHidden());
    static float applied_fov = 100.0f;   // the projection is rebuilt only when the view is marked dirty
    const float fov = want ? pm.Fov() : 100.0f;
    if (fov != applied_fov) { applied_fov = fov; RefreshView(); }
    if (want && !frozen_) { frozen_ms_ = RunningMs(); frozen_ = true; std::cout << "[Photo] On (game clock frozen)" << std::endl; }
    else if (!want && frozen_) { paused_ms_ += RunningMs() - frozen_ms_; frozen_ = false; std::cout << "[Photo] Off" << std::endl; }
    if (frozen_) { const int n = steps_.exchange(0); if (n > 0) frozen_ms_ += 34 * n; }
    else steps_ = 0;
}

// The game's name hash (0x4A01F238 -> 0x4A022748): lower-cased djb2.
// The binary's embedded app.icf (XE3U config section): S3E's own defaults, then the game's [GX]/[MGS]/... settings.
// Only unconditional blocks ({} / {ID=ANY} / {CLASS=ANY}) and {OS=NGI} / {OS=SYMBIAN} apply; per-device
// ({ID=...}, {OS=BREW}, ...) and {[MGS] X==Y} blocks (DeployConfig, KDDISpecific, SecureDemo are all 0) don't.
// Later lines override earlier ones, like the real loader.
static std::string IcfLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
static std::string IcfTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
void S3ERuntime::SetIcf(const std::string& text) {
    icf_.clear();
    std::string section;
    bool active = true;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = IcfTrim(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (line.empty() || line[0] == '#' || line.compare(0, 2, "//") == 0) continue;
        if (line[0] == '{') {
            std::string cond;
            for (char c : line.substr(1, line.find('}') - 1)) if (c != ' ' && c != '\t') cond += static_cast<char>(toupper(static_cast<unsigned char>(c)));
            active = cond.empty() || cond == "ID=ANY" || cond == "CLASS=ANY" || cond == "OS=NGI" || cond == "OS=SYMBIAN";
            continue;
        }
        if (line[0] == '[') { section = IcfLower(line.substr(1, line.find(']') - 1)); continue; }
        if (!active) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = IcfLower(IcfTrim(line.substr(0, eq))), val = IcfTrim(line.substr(eq + 1));
        if (!val.empty() && val[0] == '"') { size_t q = val.find('"', 1); val = val.substr(1, q == std::string::npos ? std::string::npos : q - 1); }
        else { size_t h = val.find('#'); if (h != std::string::npos) val = IcfTrim(val.substr(0, h)); }
        icf_[section + "/" + key] = val;
    }
    std::cout << "[Config] Embedded app.icf: " << icf_.size() << " settings" << std::endl;
}
bool S3ERuntime::IcfLookup(const std::string& group, const std::string& key, std::string& out) const {
    const std::string k = IcfLower(group) + "/" + IcfLower(key);
    auto it = icf_.find(k);
    if (it == icf_.end()) return false;
    if (const char* ex = getenv("MGS_ICF_EXCLUDE")) {   // debug: "mgs/maxparticles,gx/numtpages" = treat as unset
        const std::string list = "," + IcfLower(ex) + ",";
        if (list.find("," + k + ",") != std::string::npos) return false;
    }
    out = it->second;
    return true;
}

// Debug/override for s3eConfigGetInt/String: MGS_CONFIG="GX/Transform=HW;GX/DataCacheSizeHW=16384"
static bool ConfigOverride(const std::string& sec, const std::string& key, std::string& out) {
    const char* ov = getenv("MGS_CONFIG");
    if (!ov) return false;
    std::string all = ov, want = sec + "/" + key + "=";
    bool found = false;
    for (size_t p = 0; p < all.size();) {
        size_t e = all.find(';', p);
        std::string item = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (item.compare(0, want.size(), want) == 0) { out = item.substr(want.size()); found = true; }
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return found;
}

static uint32_t GameHash(const char* s) {
    uint32_t h = 5381;
    for (; *s; ++s) { uint8_t c = static_cast<uint8_t>(*s); if (c - 'A' < 26u) c += 32; h = h * 33 + c; }
    return h;
}

// True while the top game state is CGameStateMain (3D gameplay, not pause/inventory/codec/front end).
// State manager [0x4A0F0558]: stack array at +0x14, count at +0x18 (0x4A0AC344); state +4 = name hash.
bool S3ERuntime::InGameplay() {
    static const uint32_t kMain = GameHash("CGameStateMain");
    uint32_t mgr = 0, arr = 0, count = 0, state = 0, hash = 0;
    if (!uc_ || uc_->MemRead(G(0x4A0F0558), &mgr, 4) != Cpu::OK || !mgr) return false;
    if (uc_->MemRead(mgr + 0x14, &arr, 4) != Cpu::OK || uc_->MemRead(mgr + 0x18, &count, 4) != Cpu::OK || !arr || !count) return false;
    if (uc_->MemRead(arr + 4 * (count - 1), &state, 4) != Cpu::OK || !state) return false;
    if (uc_->MemRead(state + 4, &hash, 4) != Cpu::OK) return false;
    return hash == kMain;
}

// Level select. The game changes rooms with the Lua binding RoomChange(name) = 0x4A088BB0(L) (event table at
// 0x4A0F1968: {name, 0, id, fnA, fnB} records; RoomChange = event 0x8024): name = lua_tolstring(L, 1) (BL at
// 0x4A088BB8), copied into the event, which is posted through 0x4A0B1840(event, 0x4A06E434(), 0) - what the
// scripts' doors do (ingame.lua: RoomChange(room)).
// A room's script picks its entrance / camera from the previous room (ingame.lua: room history ox, ox[1] =
// current, ox[2] = previous, pushed by qx() on RoomCreate); arriving from a room that isn't a neighbour can
// leave e.g. CutToCamera with no camera (02_Armoury: null object in 0x4A0C2F10). So we run Lua in the game's
// own interpreter: ox[1] = <a neighbour of the target>; RoomChange(<target>) - luaL_loadbuffer 0x4A028944 +
// lua_pcall 0x4A04E9E4 on the main lua_State (from any lua_tolstring call, see lua_hook). Without a Lua
// state yet it falls back to calling RoomChange directly (room_hook substitutes the name).
static constexpr uint32_t kLuaLoadBuffer = 0x4A028945, kLuaPcall = 0x4A04E9E5;

// Level select, in two chunks run back-to-back (luachunk.h; no game code runs in between):
//  1. read: iw (the playing cutscene), CharacterIsAlive("Snake") and the story progress (story.h).
//     A cutscene thread left running carries on into the new room and asks for the old room's cameras
//     (CutToCamera -> null -> crash in 0x4A0C2F10), so the jump waits while iw is set or Snake is dead.
//  2. apply: bring the story up to the target room's stage (Story::AddApply), then what a door does before
//     RoomChange (ingame.lua Vw): nd() (cancels the codec thread); ox[1] = via (the room history).
// RoomChange itself is then called directly: posted from lua_pcall on the main state, room creation crashed
// at 0x4A05C4E2 (null from 0x4A0D6034).

void S3ERuntime::RequestRoomChange(const std::string& room, const std::string& via) {
    std::lock_guard<std::mutex> lock(room_mutex_);
    room_request_ = room;
    room_via_ = via;
}

// The main lua_State from any state the game used: G = L->l_G (+0x10); G->mainthread is a thread (tt 8 at
// +4) whose l_G is G. Found by scanning G once (layout depends on the build's lua_Number = int32).
uint32_t S3ERuntime::LuaMainState(Cpu* uc) {
    if (lua_main_ || !lua_seen_) return lua_main_;
    uint32_t g = 0;
    if (uc->MemRead(lua_seen_ + 0x10, &g, 4) != Cpu::OK || !g) return 0;
    for (uint32_t off = 0x50; off < 0x100; off += 4) {
        uint32_t p = 0, pg = 0; uint8_t tt = 0;
        if (uc->MemRead(g + off, &p, 4) != Cpu::OK || !p) continue;
        if (uc->MemRead(p + 4, &tt, 1) != Cpu::OK || tt != 8) continue;
        if (uc->MemRead(p + 0x10, &pg, 4) != Cpu::OK || pg != g) continue;
        lua_main_ = p;
        std::cout << "[LevelSelect] Lua main state 0x" << std::hex << p << " (G 0x" << g << " +0x" << off << ")" << std::dec << std::endl;
        break;
    }
    return lua_main_;
}

void S3ERuntime::PumpRoomChange(Cpu* uc) {
    in_gameplay_ = InGameplay();
    gameplay_frames_ = in_gameplay_ ? gameplay_frames_ + 1 : 0;
    std::string room, via;
    {
        std::lock_guard<std::mutex> lock(room_mutex_);
        if (room_request_.empty() || room_change_active_ || in_cb_ || lua_busy_) return;
        if (gameplay_frames_ < 30) return;   // keep it until gameplay has run a moment
        room.swap(room_request_);
        via.swap(room_via_);
    }
    if (!room_name_buf_) room_name_buf_ = GuestAlloc(64);   // the room name (room_hook)
    room.resize((std::min)(room.size(), size_t(63)));
    via.resize((std::min)(via.size(), size_t(63)));
    room_change_active_ = true;
    uc->MemWrite(room_name_buf_, room.c_str(), room.size() + 1);
    std::cout << "[LevelSelect] RoomChange(\"" << room << "\")" << (via.empty() ? "" : ", arriving from " + via) << std::endl;
    auto change = [this] {   // RoomChange directly (room_hook hands it room_name_buf_)
        lua_busy_ = false;
        pending_cbs_.push_back({G(0x4A088BB1), 0, 0, nullptr, [this](uint32_t) { room_change_active_ = false; }});
    };
    if (via.empty() || !LuaMainState(uc)) { change(); return; }
    auto retry = [this, room, via] {
        std::cout << "[LevelSelect] cutscene / codec running, retrying" << std::endl;
        {
            std::lock_guard<std::mutex> lock(room_mutex_);
            if (room_request_.empty()) { room_request_ = room; room_via_ = via; }
        }
        gameplay_frames_ = 0;   // look again in ~half a second
        room_change_active_ = false;
        lua_busy_ = false;
    };
    LuaChunk read;
    read.ReturnGlobal("iw").ReturnCall("CharacterIsAlive", {"Snake"});
    Story::AddReads(read);
    lua_busy_ = true;
    ExecLua(uc, read.Build(), read.Results(), [this, uc, room, via, change, retry](const std::vector<LuaValue>& r) {
        if (r.empty()) { change(); return; }   // the chunk failed (logged): plain RoomChange
        if (!r[0].IsNil() || !r[1].Truthy()) { retry(); return; }
        const Story::State cur = Story::ParseReads(r, 2, LayerNumber());
        LuaChunk apply;
        if (cur.valid) {
            const int target = Story::TargetStage(room, cur.CurrentStage());
            if (target != cur.CurrentStage())
                std::cout << "[LevelSelect] Story: " << Story::StageName(cur.CurrentStage()) << " -> " << Story::StageName(target) << std::endl;
            Story::AddApply(apply, cur, target);
        }
        apply.Call("nd").SetField("ox", 1, via);
        ExecLua(uc, apply.Build(), 0, [change](const std::vector<LuaValue>&) { change(); });
    });
}

// Host Lua requests (RunLua): queued from any thread, started once per frame in gameplay.
void S3ERuntime::RunLua(const LuaChunk& chunk, LuaDone done) {
    std::lock_guard<std::mutex> lock(lua_mutex_);
    lua_queue_.push_back({chunk.Build(), chunk.Results(), std::move(done)});
}

bool S3ERuntime::LuaPending() {
    std::lock_guard<std::mutex> lock(lua_mutex_);
    return in_gameplay_ && !lua_queue_.empty();
}

void S3ERuntime::PumpLua(Cpu* uc) {
    if (frame_tick_) frame_tick_(in_gameplay_);
    if (!in_gameplay_ || lua_busy_ || in_cb_ || room_change_active_ || !LuaMainState(uc)) return;
    LuaRequest req;
    {
        std::lock_guard<std::mutex> lock(lua_mutex_);
        if (lua_queue_.empty()) return;
        req = std::move(lua_queue_.front());
        lua_queue_.pop_front();
    }
    lua_busy_ = true;
    // Each request's done starts the next one, so the whole queue runs before the game resumes.
    auto run = std::make_shared<std::function<void(LuaRequest)>>();
    *run = [this, uc, run](LuaRequest r) {
        LuaDone user = std::move(r.done);
        ExecLua(uc, r.chunk, r.nresults, [this, run, user](const std::vector<LuaValue>& res) {
            if (user) user(res);
            LuaRequest next;
            {
                std::lock_guard<std::mutex> lock(lua_mutex_);
                if (lua_queue_.empty()) { lua_busy_ = false; return; }
                next = std::move(lua_queue_.front());
                lua_queue_.pop_front();
            }
            (*run)(std::move(next));
        });
    };
    (*run)(std::move(req));
}

// Loads and runs one chunk on the main lua_State as queued guest callbacks (luaL_loadbuffer, then lua_pcall
// with nresults), then done(results) from the pcall's completion; nothing else of the game runs in between.
// Results are TValues {value, tt} (8 bytes) from the old top; L->top is restored afterwards. The stack can be
// reallocated by the call (L->stack at +32 moves), so the old top is kept as an offset from L->stack.
void S3ERuntime::ExecLua(Cpu* uc, const std::string& chunk, int nresults, LuaDone done) {
    const uint32_t L = LuaMainState(uc);
    if (!L) { if (done) done({}); return; }
    const uint32_t need = static_cast<uint32_t>(chunk.size()) + 8;
    if (lua_buf_cap_ < need) { lua_buf_cap_ = (std::max<uint32_t>)(need, 8192u); lua_buf_ = GuestAlloc(lua_buf_cap_); }
    const uint32_t name = lua_buf_ + static_cast<uint32_t>(chunk.size());
    uc->MemWrite(lua_buf_, chunk.data(), chunk.size());
    uc->MemWrite(name, "=host", 6);
    auto stack = [uc, L] { uint32_t st = 0; uc->MemRead(L + 32, &st, 4); return st; };   // L->stack
    uint32_t top0 = 0;
    uc->MemRead(L + 8, &top0, 4);   // L->top: restored afterwards (drops an error message, if any)
    const uint32_t top_off = top0 - stack();
    auto top = [stack, top_off] { return stack() + top_off; };
    auto fail = [uc, L](const char* what, uint32_t r) {
        uint32_t t = 0, v = 0, tt = 0;   // error message: TValue at top-1; TString data at +16
        char msg[160] = {};
        uc->MemRead(L + 8, &t, 4);
        uc->MemRead(t - 8, &v, 4); uc->MemRead(t - 4, &tt, 4);
        if (tt == 4) uc->MemRead(v + 16, msg, sizeof(msg) - 1);
        std::cout << "[Lua] " << what << " failed (" << r << "): " << msg << std::endl;
    };
    PendingCallback load{G(kLuaLoadBuffer), L, lua_buf_, nullptr, nullptr, static_cast<uint32_t>(chunk.size()), name};
    load.on_done = [this, uc, L, top, fail, nresults, done](uint32_t r) {
        if (r != 0) { fail("luaL_loadbuffer", r); const uint32_t t = top(); uc->MemWrite(L + 8, &t, 4); if (done) done({}); return; }
        PendingCallback call{G(kLuaPcall), L, 0, nullptr, nullptr, static_cast<uint32_t>(nresults), 0};
        call.on_done = [uc, L, top, fail, nresults, done](uint32_t r2) {
            std::vector<LuaValue> res;
            if (r2 != 0) fail("lua_pcall", r2);
            else {
                for (int i = 0; i < nresults; ++i) {
                    uint32_t tv[2] = {};
                    uc->MemRead(top() + 8 * i, tv, 8);
                    LuaValue v;
                    if (tv[1] == LuaValue::Bool) v = LuaValue(tv[0] != 0);
                    else if (tv[1] == LuaValue::Number) v = LuaValue(static_cast<int>(tv[0]));
                    else if (tv[1] == LuaValue::String) {
                        uint32_t len = 0;
                        uc->MemRead(tv[0] + 12, &len, 4);
                        std::string str((std::min<uint32_t>)(len, 256u), '\0');
                        if (!str.empty()) uc->MemRead(tv[0] + 16, &str[0], str.size());
                        v = LuaValue(str);
                    } else if (tv[1] != 0) {   // a table, function, thread...: report it as `true`
                        v = LuaValue(true);
                    }
                    res.push_back(v);
                }
            }
            const uint32_t t = top();
            uc->MemWrite(L + 8, &t, 4);
            if (done) done(res);
        };
        pending_cbs_.push_front(std::move(call));
    };
    pending_cbs_.push_front(std::move(load));
}

// SetLayerNumber (0x4A031CC0) stores the layer at [0x4A06E434() + 0xC4]; 0x4A06E434 returns [0x4A0F1BF0].
int S3ERuntime::LayerNumber() {
    uint32_t env = 0, layer = 0;
    if (!uc_ || uc_->MemRead(G(0x4A0F1BF0), &env, 4) != Cpu::OK || !env) return -1;
    if (uc_->MemRead(env + 0xC4, &layer, 4) != Cpu::OK) return -1;
    return static_cast<int>(layer);
}

// IwGx's frame-rate counter (GX/DisplayFramerate, read once at 0x4A021E6E): ctx flag 0x4000000 at
// [[0x4A0EF128]+0], kept in step with the setting every frame.
void S3ERuntime::ApplyShowFps() {
    uint32_t ctx = 0, flags = 0;
    if (!uc_ || uc_->MemRead(G(0x4A0EF128), &ctx, 4) != Cpu::OK || !ctx) return;
    if (uc_->MemRead(ctx, &flags, 4) != Cpu::OK) return;
    const uint32_t want = show_fps_ ? (flags | 0x4000000u) : (flags & ~0x4000000u);
    if (want != flags) uc_->MemWrite(ctx, &want, 4);
}

// "Hide soft keys" (Smart: gameplay only; All: everywhere): clear the visible flag (element +0xC, bit 0x8) of the soft-key bars'
// labels and frames (the Equip / Codec prompts and the black boxes behind them). Runs every frame: the game
// sets the flag again whenever it changes a label. In Smart mode menus, pause, inventory and codec keep
// their labels: whatever was hidden is made visible again once gameplay isn't the top state (or it's off).
void S3ERuntime::ApplySoftKeyVisibility() {
    if (!uc_) return;
    const int mode = hide_soft_keys_;
    const bool hide = mode == 2 || (mode == 1 && InGameplay());
    if (!hide) {
        for (uint32_t elem : softkeys_hidden_) {
            uint32_t flags = 0;
            if (uc_->MemRead(elem + 0xC, &flags, 4) == Cpu::OK) { flags |= 0x8u; uc_->MemWrite(elem + 0xC, &flags, 4); }
        }
        softkeys_hidden_.clear();
        return;
    }
    for (uint32_t bar : softkey_bars_) {
        for (uint32_t off : {0x78u, 0x7Cu, 0x80u, 0x84u}) {
            uint32_t elem = 0, flags = 0;
            if (uc_->MemRead(bar + off, &elem, 4) != Cpu::OK || !elem) continue;
            if (uc_->MemRead(elem + 0xC, &flags, 4) != Cpu::OK || !(flags & 0x8)) continue;
            flags &= ~0x8u;
            uc_->MemWrite(elem + 0xC, &flags, 4);
            if (std::find(softkeys_hidden_.begin(), softkeys_hidden_.end(), elem) == softkeys_hidden_.end()) softkeys_hidden_.push_back(elem);
        }
    }
}

// Keys currently held stay invisible to the game (no DOWN, no RELEASED when let go) until released.
void S3ERuntime::SwallowHeldKeys() {
    std::lock_guard<std::mutex> lock(key_mutex_);
    for (int k = 0; k < 256; ++k)
        if (phys_down_[k]) { swallowed_[k] = true; pending_pressed_[k] = pending_released_[k] = false; }
}

void S3ERuntime::SetKeyState(uint32_t key_code, bool down) {
    if (key_code >= 256) return;
    std::lock_guard<std::mutex> lock(key_mutex_);
    if (down == phys_down_[key_code]) return;          // auto-repeat / duplicate: no new edge
    phys_down_[key_code] = down;
    if (!down && swallowed_[key_code]) { swallowed_[key_code] = false; return; }   // its press was cleared
    (down ? pending_pressed_ : pending_released_)[key_code] = true;
}

// Debug: ring buffer of the last 64 syscalls, dumped on exit (shows what a stalled game is doing)
struct RecentCall { const std::string* name; uint32_t lr, r0, r1, r2, r3, ret; };
static RecentCall g_recent[64]; static uint32_t g_recent_n = 0;
// Debug: MGS_TRACE_FILE=<substring> logs every s3eFile* call on handles whose path contains it.
static std::set<uint32_t> g_traced_handles;

void S3ERuntime::RequestQuit() {
    quit_requested_ = true;
    if (uc_) uc_->Stop();
}

// Debug summary printed at exit when MGS_EXIT_DIAG is set (call after the emu thread has stopped):
// per-import call counts, heap use, break hits, MGS_PEEK words and the last 64 syscalls.
void S3ERuntime::DumpExitDiagnostics(bool force) {
    if (!force && !getenv("MGS_EXIT_DIAG")) return;
    for (size_t i = 0; i < g_hist.size() && i < imports_.size(); ++i)
        if (g_hist[i]) std::cout << "[Hist] " << imports_[i] << " " << g_hist[i] << '\n';
    std::cout << "[Heap] in use " << heap_.BytesInUse() / 1024 << " KB, peak " << heap_.PeakInUse() / 1024
              << " KB of " << HEAP_SIZE / 1024 << " KB\n";
    if (g_break_hits) std::cout << "[Break] total hits " << g_break_hits << '\n';
    if (const char* pk = getenv("MGS_PEEK")) { // debug: MGS_PEEK=<hex addr>,<words> dumps guest memory at exit
        uint32_t a = strtoul(pk, nullptr, 16), n = 8;
        if (const char* c = strchr(pk, ',')) n = strtoul(c + 1, nullptr, 10);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t v = 0; uc_->MemRead(a + 4 * i, &v, 4);
            std::cout << "[Peek] 0x" << std::hex << a + 4 * i << " = 0x" << v << std::dec << '\n';
        }
    }
    for (uint32_t i = g_recent_n > 64 ? g_recent_n - 64 : 0; i < g_recent_n; ++i) {
        const RecentCall& c = g_recent[i % 64];
        std::cout << "[Recent] " << *c.name << std::hex << " lr=0x" << c.lr << " r0=0x" << c.r0 << " r1=0x" << c.r1
                  << " r2=0x" << c.r2 << " r3=0x" << c.r3 << " -> 0x" << c.ret << std::dec << '\n';
    }
    std::cout << std::flush;
}

void S3ERuntime::IntrHook(Cpu* uc, uint32_t intno, void* user_data) {
    uint32_t pc = 0;
    uc->RegRead(Cpu::REG_PC, &pc);
    uint32_t func_idx = (pc - 4 - TRAMPOLINE_BASE) / 8;

    auto* runtime = reinterpret_cast<S3ERuntime*>(user_data);
    runtime->HandleSyscall(uc, func_idx);
}

void S3ERuntime::HandleSyscall(Cpu* uc, uint32_t func_idx) {
    {   // debug: MGS_DIAG_AFTER=<s> prints the MGS_EXIT_DIAG summary once, that many seconds in (no clean exit needed)
        static const double after = getenv("MGS_DIAG_AFTER") ? atof(getenv("MGS_DIAG_AFTER")) : 0;
        static bool done = false;
        if (after > 0 && !done && std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count() > after) {
            done = true;
            DumpExitDiagnostics(true);
        }
    }
    if (func_idx == imports_.size() && in_cb_) {
        // Guest callback finished: report its result, chain any callbacks it queued, then restore the
        // interrupted syscall's return state
        in_cb_ = false;
        if (running_cb_done_) {
            uint32_t cb_ret = 0;
            uc->RegRead(Cpu::REG_R0, &cb_ret);
            auto done = std::move(running_cb_done_);
            running_cb_done_ = nullptr;
            done(cb_ret);
        }
        if (DispatchPending(uc)) return;
        uc->RegWrite(Cpu::REG_R0, &saved_[0]);
        uc->RegWrite(Cpu::REG_R1, &saved_[1]);
        uc->RegWrite(Cpu::REG_R2, &saved_[2]);
        uc->RegWrite(Cpu::REG_R3, &saved_[3]);
        uc->RegWrite(Cpu::REG_R12, &saved_[4]);
        // Unicorn takes the Thumb state from bit 0 of a PC write, so write PC first, then CPSR.
        uint32_t lr = saved_[5], pc = lr & ~1u, cpsr = 0;
        uc->RegWrite(Cpu::REG_PC, &pc);
        uc->RegRead(Cpu::REG_CPSR, &cpsr);
        cpsr = (lr & 1) ? (cpsr | 0x20) : (cpsr & ~0x20u);
        uc->RegWrite(Cpu::REG_CPSR, &cpsr);
        in_cb_ = false;
        return;
    }
    if (func_idx >= imports_.size()) {
        std::cerr << "[Runtime] Syscall index out of range: " << func_idx << std::endl;
        return;
    }

    const std::string& name = imports_[func_idx];
    if (g_hist.size() < imports_.size()) g_hist.resize(imports_.size());
    if (++g_hist[func_idx] == 1 && VerboseLog()) std::cout << "[First] " << name << std::endl;
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, lr = 0;
    uint32_t* const regfile = uc->RegisterFile();   // (native backend: plain reads)
    if (regfile) { r0 = regfile[0]; r1 = regfile[1]; r2 = regfile[2]; r3 = regfile[3]; lr = regfile[14]; }
    else {
        uc->RegRead(Cpu::REG_R0, &r0);
        uc->RegRead(Cpu::REG_R1, &r1);
        uc->RegRead(Cpu::REG_R2, &r2);
        uc->RegRead(Cpu::REG_R3, &r3);
        uc->RegRead(Cpu::REG_LR, &lr);
    }

    uint32_t ret_val = 0;

    static const bool trace_sound = getenv("MGS_TRACE_SOUND") != nullptr;
    if (trace_sound && (name.compare(0, 8, "s3eSound") == 0 || name.compare(0, 8, "s3eAudio") == 0)) {
        uint32_t sp = 0, s0 = 0, s1 = 0;
        uc->RegRead(Cpu::REG_SP, &sp);
        uc->MemRead(sp, &s0, 4); uc->MemRead(sp + 4, &s1, 4);
        std::cout << "[Snd] " << name << std::hex << "(" << r0 << ", " << r1 << ", " << r2 << ", " << r3
                  << " | " << s0 << ", " << s1 << ") lr=" << lr << std::dec << std::endl;
    }

    static const bool trace_keys = getenv("MGS_TRACE_KEYS") != nullptr;   // debug: keyboard calls that see input
    if (trace_keys && name == "s3eKeyboardGetState" && r0 < 256) {   // first query of each key code, with caller
        static bool seen[256];
        if (!seen[r0]) { seen[r0] = true; std::cout << "[Keys] first query of key 0x" << std::hex << r0 << " lr=0x" << lr << std::dec << std::endl; }
    }
    if (trace_keys && name.compare(0, 11, "s3eKeyboard") == 0) {
        bool any = false;
        for (int k = 0; k < 256 && !any; ++k) any = phys_down_[k] || frame_down_[k] || frame_pressed_[k] || frame_released_[k] || pending_pressed_[k] || pending_released_[k];
        if (any || name == "s3eKeyboardClearState") {
            uint32_t st = 0;
            if (name == "s3eKeyboardGetState" && r0 < 256) st = (frame_down_[r0] ? 1 : 0) | (frame_pressed_[r0] ? 2 : 0) | (frame_released_[r0] ? 4 : 0);
            if (name != "s3eKeyboardGetState" || st)
                std::cout << "[Keys] " << name << std::hex << " r0=0x" << r0 << " state=" << st << " lr=0x" << lr << std::dec << std::endl;
        }
    }
    const uint32_t regs[4] = {r0, r1, r2, r3};
    if ((name[0] == 'g' || name[0] == 'e') && GLES::Instance().Handle(name, uc, regs, ret_val)) {
        // serviced by gles.cpp
    }
    else if (name == "s3eExtGet") {
        if (VerboseLog()) std::cout << "[Ext] s3eExtGet(" << ReadGuestString(uc, r0, 64) << ") -> unavailable" << std::endl;
        ret_val = 1; // S3E_RESULT_ERROR (extension unavailable)
    }
    else if (name == "s3eMalloc") {
        ret_val = GuestAlloc(r0);
    }
    else if (name == "s3eRealloc" || name == "s3eReallocBase") {
        const uint32_t old_ptr = r0, new_size = r1;
        const uint32_t old_size = heap_.SizeOf(old_ptr);
        uint32_t copy_from = 0, copy_len = 0;
        ret_val = heap_.Realloc(old_ptr, new_size, &copy_from, &copy_len);
        if (ret_val && copy_from) {                  // moved: copy the payload, free the old block
            copy_len = (std::min)(copy_len, new_size);
            std::vector<uint8_t> tmp(copy_len);
            uc->MemRead(copy_from, tmp.data(), copy_len);
            uc->MemWrite(ret_val, tmp.data(), copy_len);
            heap_.Free(copy_from);
            ZeroGuest(uc, ret_val + copy_len, heap_.SizeOf(ret_val) - copy_len);
        } else if (ret_val && ret_val == old_ptr && heap_.SizeOf(ret_val) > old_size) {
            ZeroGuest(uc, ret_val + old_size, heap_.SizeOf(ret_val) - old_size); // grown in place
        } else if (ret_val && !old_ptr) {
            ZeroGuest(uc, ret_val, heap_.SizeOf(ret_val));
        }
        if (!ret_val && new_size) std::cerr << "[Heap] Out of guest memory reallocating " << new_size << " bytes" << std::endl;
    }
    else if (name == "s3eFree" || name == "s3eFreeBase") {
        heap_.Free(r0);
    }
    else if (name == "s3eMemoryGetInt") {
        // The game sizes its own memory pool from this (s3eMemorySetUserMemMgr at 0x4A02FAE8; property
        // wrappers at 0x4A098800..0x4A098840) and takes nearly all of it. Report a phone-sized 32 MB heap so
        // the pool stays modest and the rest of the 128 MB guest heap serves everything else.
        constexpr uint32_t kDeviceHeap = 32 * 1024 * 1024;
        switch (r0) {
            case 1:  ret_val = kDeviceHeap; break;                                              // size
            case 2:  ret_val = (std::min)(kDeviceHeap, heap_.FreeBytes()); break;               // free
            case 3:  ret_val = heap_.BytesInUse(); break;                                       // used
            case 4:  ret_val = (std::min)(kDeviceHeap, heap_.LargestFree()); break;             // largest free block
            default: ret_val = 0; break;
        }
        if (VerboseLog()) std::cout << "[Heap] s3eMemoryGetInt(" << r0 << ") -> " << ret_val << std::endl;
    }
    else if (name == "s3eMemorySetUserMemMgr") {
        ret_val = 0;
    }
    else if (name == "s3eTimerGetMs") {
        ret_val = static_cast<uint32_t>(GameMs());
    }
    else if (name == "s3eTimerGetInt") {
        // 0 = S3E_TIMER_RESOLUTION (ms). The game does `period % resolution`
        // at 0x4A06A4A0, so returning 0 caused the "Divide by zero" errors.
        ret_val = (r0 == 0) ? 1 : 0;
    }
    else if (name == "s3eDeviceGetInt") {
        ret_val = 0;
    }
    else if (name == "s3eDeviceRegister") {   // (id, fn, userdata): PAUSE 0x4A0489B9, UNPAUSE 0x4A04DC89, EXIT 0x4A025B69
        device_cbs_[r0] = {r1, r2};
        if (VerboseLog()) std::cout << "[Device] Register(" << r0 << ", 0x" << std::hex << r1 << ", 0x" << r2 << ")" << std::dec << std::endl;
        ret_val = 0;
    }
    else if (name == "s3eDeviceUnRegister") {   // (id, fn)
        auto it = device_cbs_.find(r0);
        if (it != device_cbs_.end() && it->second.first == r1) device_cbs_.erase(it);
        ret_val = 0;
    }
    else if (name == "s3eDeviceYield") {
        ApplyDisplay(uc, true);
        ApplySoftKeyVisibility();
        if (r0 > 0) {
            // At most 1 ms: the game yields a fixed 10 ms every frame (for the phone's OS), and GLES::Present
            // already paces frames to 60 fps. Fast hosts lose nothing; on Android the full 10 ms on top of the
            // emulation work held the game at ~46 fps.
#ifdef MGS_3DS
            std::this_thread::yield();   // 3DS: nothing else to give the time to; a sleep cost ~5 ms a frame
#else
            std::this_thread::sleep_for(std::chrono::milliseconds((std::min<uint32_t>)(r0, 1u)));
#endif
        } else {
            std::this_thread::yield();
        }
        ret_val = 0;
    }
    else if (name == "s3eTimerSetTimer") {
        auto now = GameMs();
        timer_ = { true, static_cast<uint32_t>(now) + r0, r1, r2 };
        if (VerboseLog()) std::cout << "[Timer] Set " << r0 << "ms fn=0x" << std::hex << r1 << std::dec << std::endl;
        ret_val = 0;
    }
    else if (name == "s3eDeviceExit") {
        // Never returns on a device (the OS ends the app); code after the call is not meant to run.
        std::cout << "[Runtime] Game called s3eDeviceExit, quitting" << std::endl;
        RequestQuit();
        if (exit_callback_) exit_callback_();
        ret_val = 0;
    }
    else if (name == "s3eDeviceCheckQuitRequest") {
        ret_val = quit_requested_ ? 1 : 0;
    }
    else if (name == "s3eSurfacePtr") {
        ret_val = FB_BASE;
    }
    else if (name == "s3eSurfaceGetInt") {
        if (!display_init_) ApplyDisplay(uc, false);
        if (r0 == 0)       ret_val = surf_w_;       // S3E_SURFACE_WIDTH
        else if (r0 == 1)  ret_val = surf_h_;       // S3E_SURFACE_HEIGHT
        else if (r0 == 2)  ret_val = surf_w_ * 2;   // S3E_SURFACE_PITCH (RGB565)
        else if (r0 == 3)  ret_val = 0;             // blit direction: the screen is never rotated, see ApplyDisplay
        else if (r0 == 8)  ret_val = 1;             // pixel type (RGB565)
        else if (r0 == 11) ret_val = 0;             // device blit direction (0x4A028332 -> 0x4A032720)
        else               ret_val = 0;
    }
    else if (name == "s3eSurfaceRegister") {
        if (r0 == 1) surface_cbs_.push_back({r1, r2}); // screen size/orientation change (IwGx 0x4A02DF61, game 0x4A031D6D)
        ret_val = 0;
    }
    else if (name == "s3eSurfaceSetup") {
        ret_val = 0;
    }
    else if (name == "s3eSurfaceShow") {
        frame_count_++;
        if (frame_callback_) {
            std::vector<uint16_t> fb(static_cast<size_t>(surf_w_) * surf_h_);
            uc->MemRead(FB_BASE, fb.data(), fb.size() * sizeof(uint16_t));
            frame_callback_(fb.data(), surf_w_, surf_h_);
        }
        ret_val = 0;
    }
    else if (name == "s3eConfigGetInt") {
        std::string sec = ReadGuestString(uc, r0, 32);
        std::string key = ReadGuestString(uc, r1, 32);
        uint32_t val = 0;
        bool found = false;

        if (sec == "GX") {
            if (key == "PolyBufferSize")   { val = 120000; found = true; }
            else if (key == "PolyOTSize")  { val = 5120;   found = true; }
            else if (key == "ZDepthOfsSafety") { val = 0;  found = true; }
            else if (key == "OTBucketShift")   { val = 0;  found = true; }
            else if (key == "NumTPages")       { val = 16; found = true; } // 8 overflowed on the first level ("Out of T-Pages")
            else if (key == "VertCacheSize")   { val = 6000;  found = true; }
            else if (key == "VertCacheSizeHW") { val = 12000; found = true; }
            // IwGx's per-frame data cache (bump-allocated at [[0x4A0EF128]+0x250] with no bounds checks, set up at
            // 0x4A021EFA -> 0x4A0389D0). Unset it defaults to 16 KB, which busy rooms overflow (heap corruption,
            // then Cpu::ERR_WRITE_UNMAPPED around 0x4A080968). The icf says 300000; widescreen / a wider FOV draw
            // more per frame, so allow more.
            else if (key == "DataCacheSize" || key == "DataCacheSizeHW") { val = 1000000; found = true; }
        }
        // The icf puts the soft keys down the right-hand side in landscape; keep them at the bottom (our layout,
        // the soft-key bar hook and the arbitrary-resolution handling all assume that).
        if (sec == "MGS" && key == "SoftkeysOnRhsInLandscape") { val = 0; found = true; }
        // The icf's 0 keeps saves in s3eSecureStorage (unimplemented -> "Data corrupted"); unset, the game
        // defaults to files (savedata.i3d / savedata2.i3d), which is where the existing saves are.
        if (sec == "MGS" && key == "LoadSaveFromFile") { val = 1; found = true; }
        // No phone camera here. The icf's 1 starts a fresh install with the camera motion-control test, and the
        // save written then reads back as "Data corrupted" on the next launch.
        if (sec == "MGS" && key == "CameraEnabled") { val = 0; found = true; }
        std::string icf;
        if (ConfigOverride(sec, key, icf)) { val = static_cast<uint32_t>(strtol(icf.c_str(), nullptr, 0)); found = true; }
        else if (!found && IcfLookup(sec, key, icf)) { val = static_cast<uint32_t>(strtol(icf.c_str(), nullptr, 0)); found = true; }
        {
            static std::set<std::string> logged;
            if (logged.insert(sec + "/" + key).second && VerboseLog())
                std::cout << "[Config] " << sec << "/" << key << " -> " << (found ? std::to_string(static_cast<int32_t>(val)) : "(unset)") << std::endl;
        }

        if (found && r2 != 0) {
            uc->MemWrite(r2, &val, 4);
            ret_val = 0; // S3E_RESULT_SUCCESS
        } else {
            ret_val = 1; // S3E_RESULT_ERROR
        }
    }
    else if (name == "s3eConfigGetString") { // (group, name, out[S3E_CONFIG_STRING_MAX])
        std::string sec = ReadGuestString(uc, r0, 32);
        std::string key = ReadGuestString(uc, r1, 32);
        std::string val;
        bool found = false;
        // GX/Rasterisation=HW selects the OpenGL ES renderer (checked at 0x4A02F50A). The level data is
        // binarised for it, so the software path cannot load levels.
        if (sec == "GX" && key == "Rasterisation" && UseGL()) { val = "HW"; found = true; }
        // Resource build style: selects the binarised data variant (0x4A078E92 -> 0x4A078BDC). The shipped
        // assets are "gles1"; without it CIwModel takes the software layout and level groups fail to parse.
        if (sec == "RESMANAGER" && key == "ResBuildStyle" && UseGL()) { val = "gles1"; found = true; }
        // Not from the icf: GX/Rasterisation (it says "HW or SW"), and GX/Transform / GX/Lighting ("SW" = software
        // T&L; with it the Mess Hall corrupts IwGx's slot list -> Cpu::ERR_READ_UNMAPPED at 0x4A02915C). Unset, the
        // engine uses GL transform and lighting, which is what everything was verified with.
        const bool icf_ok = !(sec == "GX" && (key == "Rasterisation" || key == "Transform" || key == "Lighting"));
        if (!found && icf_ok) found = IcfLookup(sec, key, val);
        // Debug/override: MGS_CONFIG="GX/Transform=HW;GX/Lighting=SW"
        if (ConfigOverride(sec, key, val)) found = true;
        if (found) uc->MemWrite(r2, val.c_str(), val.size() + 1);
        if (VerboseLog()) std::cout << "[Config] " << sec << "/" << key << " -> " << (found ? val : "(unset)") << std::endl;
        ret_val = found ? 0 : 1;
    }
    else if (name == "s3eGLGetInt") {
        if (r0 == 0) ret_val = UseGL() ? 0x110 : 0; // S3E_GL_VERSION: GLES 1.1 (must be > 0xFF, see 0x4A02F528)
        else ret_val = 0;
        if (VerboseLog()) std::cout << "[GL] s3eGLGetInt(" << r0 << ") -> 0x" << std::hex << ret_val << std::dec << std::endl;
    }
    else if (name == "s3eGLGetNativeWindow") {
        ret_val = 1;
    }
    // Keyboard: the game only sees the snapshot taken by its last s3eKeyboardUpdate.
    // State bits: 1 = DOWN, 2 = PRESSED (went down since the previous update), 4 = RELEASED (went up).
    // A tap shorter than a frame reports PRESSED|RELEASED, so it is never lost.
    else if (name == "s3eKeyboardGetState") {
        ret_val = 0;
        if (r0 < 256) {
            std::lock_guard<std::mutex> lock(key_mutex_);
            ret_val = (frame_down_[r0] ? 1 : 0) | (frame_pressed_[r0] ? 2 : 0) | (frame_released_[r0] ? 4 : 0);
        }
    }
    else if (name == "s3eKeyboardAnyKey") {   // first key down or pressed this frame (0 = none)
        std::lock_guard<std::mutex> lock(key_mutex_);
        ret_val = 0;
        for (uint32_t k = 1; k < 256; ++k)
            if (frame_down_[k] || frame_pressed_[k]) { ret_val = k; break; }
    }
    else if (name == "s3eKeyboardUpdate") {
        UpdatePhotoMode();
        ApplySoftKeyVisibility();
        ApplyShowFps();
        PumpRoomChange(uc);
        PumpLua(uc);
        std::lock_guard<std::mutex> lock(key_mutex_);
        for (uint32_t k = 0; k < 256; ++k) {
            frame_down_[k] = phys_down_[k] && !swallowed_[k];
            frame_pressed_[k] = pending_pressed_[k];
            frame_released_[k] = pending_released_[k];
            pending_pressed_[k] = pending_released_[k] = false;
        }
        ret_val = 0;
    }
    else if (name == "s3eKeyboardClearState") {
        // Drop queued edges and the snapshot; keys still held are swallowed until released (no DOWN, and no
        // RELEASED when let go), so a press that ended the previous screen can't act on the next one.
        SwallowHeldKeys();
        std::lock_guard<std::mutex> lock(key_mutex_);
        memset(pending_pressed_, 0, sizeof(pending_pressed_));
        memset(pending_released_, 0, sizeof(pending_released_));
        memset(frame_down_, 0, sizeof(frame_down_));
        memset(frame_pressed_, 0, sizeof(frame_pressed_));
        memset(frame_released_, 0, sizeof(frame_released_));
        ret_val = 0;
    }
    else if (name == "s3eFileCheckExists") {
        std::string path = ReadGuestString(uc, r0);
        ret_val = VirtualFileSystem::Instance().CheckExists(path) ? 1 : 0;
        if (!ret_val && VerboseLog()) std::cout << "  [VFS] CheckExists('" << path << "') -> no" << std::endl;
    }
    else if (name == "s3eFileOpen") {
        std::string path = ReadGuestString(uc, r0);
        std::string mode = ReadGuestString(uc, r1);
        ret_val = VirtualFileSystem::Instance().Open(path, mode);
        static const char* trace = getenv("MGS_TRACE_FILE");
        if (ret_val && trace && path.find(trace) != std::string::npos) {
            g_traced_handles.insert(ret_val);
            std::cout << "[FTrace] open " << path << " -> 0x" << std::hex << ret_val << " lr=0x" << lr << " bt:";
            uint32_t sp = 0, words[128] = {};   // rough backtrace: Thumb return addresses on the stack
            uc->RegRead(Cpu::REG_SP, &sp);
            uc->MemRead(sp, words, sizeof(words));
            for (uint32_t w : words) if ((w & 1) && w >= G(0x4A000000) && w < G(0x4A0F2000)) std::cout << " " << w;
            std::cout << std::dec << std::endl;

        }
    }
    else if (name == "s3eFileClose") {
        VirtualFileSystem::Instance().Close(r0);
        ret_val = 0;
    }
    else if (name == "s3eFileRead") {
        uint32_t buf = r0;
        uint32_t elem_size = r1;
        uint32_t count = r2;
        uint32_t handle = r3;
        if (elem_size > 0 && count > 0) {
            size_t total = static_cast<size_t>(elem_size) * count;
            static std::vector<uint8_t> tmp;   // (reused: hundreds of small reads a frame while loading)
            if (tmp.size() < total) tmp.resize(total);
            uint32_t read_cnt = VirtualFileSystem::Instance().Read(handle, tmp.data(), elem_size, count);
            if (read_cnt > 0) {
                static const uint64_t watch = getenv("MGS_WATCH") ? strtoull(getenv("MGS_WATCH"), nullptr, 16) : 0;
                if (watch && watch >= buf && watch < uint64_t(buf) + read_cnt * elem_size) {
                    uint32_t v = 0; memcpy(&v, tmp.data() + (watch - buf), (std::min<uint32_t>)(4u, uint32_t(buf + read_cnt * elem_size - watch)));
                    std::cout << "[Watch] s3eFileRead into 0x" << std::hex << watch << " = 0x" << v << " (handle 0x" << handle
                              << ", lr=0x" << lr << ")" << std::dec << std::endl;
                }
                uc->MemWrite(buf, tmp.data(), read_cnt * elem_size);
            }
            ret_val = read_cnt;
        } else {
            ret_val = 0;
        }
    }
    else if (name == "s3eFileOpenFromMemory") {
        // s3eFile* s3eFileOpenFromMemory(void* buffer, uint32 bufferLen)
        // Used by IwSerialise (0x4A0406F8) and the resource loader (0x4A07D084).
        ret_val = VirtualFileSystem::Instance().OpenFromMemory(r0, r1);
    }
    else if (name == "s3eFileWrite") {
        // uint32 s3eFileWrite(const void* buf, uint32 elemSize, uint32 noElems, s3eFile* file)
        uint32_t elem_size = r1, count = r2, handle = r3;
        ret_val = 0;
        if (elem_size > 0 && count > 0) {
            std::vector<uint8_t> tmp(static_cast<size_t>(elem_size) * count);
            uc->MemRead(r0, tmp.data(), tmp.size());
            ret_val = VirtualFileSystem::Instance().Write(handle, tmp.data(), elem_size, count);
        }
    }
    else if (name == "s3eFilePutChar") {
        // int s3eFilePutChar(char c, s3eFile* file)
        ret_val = static_cast<uint32_t>(VirtualFileSystem::Instance().PutChar(r1, static_cast<uint8_t>(r0)));
    }
    else if (name == "s3eFileReadString") {
        // char* s3eFileReadString(char* string, uint32 maxLen, s3eFile* file) â€” fgets semantics
        auto& vfs = VirtualFileSystem::Instance();
        std::string line;
        while (r1 > 0 && line.size() + 1 < r1) {
            int32_t c = vfs.GetChar(r2);
            if (c < 0) break;
            line.push_back(static_cast<char>(c));
            if (c == '\n') break;
        }
        if (line.empty() || r0 == 0) {
            ret_val = 0;
        } else {
            uc->MemWrite(r0, line.c_str(), line.size() + 1);
            ret_val = r0;
        }
    }
    else if (name == "s3eFileFlush") {
        VirtualFileSystem::Instance().Flush(r0);
        ret_val = 0;
    }
    else if (name == "s3eFileDelete") {
        ret_val = VirtualFileSystem::Instance().Delete(ReadGuestString(uc, r0)) ? 0 : 1;
    }
    else if (name == "s3eFileMakeDirectory") {
        ret_val = VirtualFileSystem::Instance().MakeDirectory(ReadGuestString(uc, r0)) ? 0 : 1;
    }
    else if (name == "s3eFileSeek") {
        uint32_t handle = r0;
        int32_t offset = static_cast<int32_t>(r1);
        int origin = static_cast<int>(r2);
        ret_val = VirtualFileSystem::Instance().Seek(handle, offset, origin);
    }
    else if (name == "s3eFileTell") {
        ret_val = static_cast<uint32_t>(VirtualFileSystem::Instance().Tell(r0));
    }
    else if (name == "s3eFileGetSize") {
        ret_val = VirtualFileSystem::Instance().GetSize(r0);
    }
    else if (name == "s3eFileEOF") {
        ret_val = VirtualFileSystem::Instance().EOF_Reached(r0) ? 1 : 0;
    }
    else if (name == "s3eFileGetChar") {
        ret_val = static_cast<uint32_t>(VirtualFileSystem::Instance().GetChar(r0));
    }
    else if (name == "s3eFileGetError") {
        ret_val = 0;
    }
    // s3eSound (S3E 3.x numbering, confirmed against the game's IwSound code):
    //   s3eSoundGetInt/SetInt: 0 = master volume (0..256), 1 = output frequency, 3 = channels, 5 = available
    //   channel props: 0 = status, 1 = rate, 2 = user var, 3 = volume (0..256), 4 = paused
    //   callbacks: 0 = END_SAMPLE {channel, repeatsRemaining}, 1 = GEN_AUDIO (s3eSoundGenAudioInfo), 3 = STOP
    else if (name == "s3eSoundGetInt") {
        switch (r0) {
            case 0:  ret_val = static_cast<uint32_t>(master_volume_); break;
            case 1:
            case 2:  ret_val = SOUND_RATE; break;
            case 3:  ret_val = MAX_SOUND_CHANNELS; break;
            case 4:  { int n = 0; for (auto& c : channels_) n += c.playing; ret_val = n; } break;
            case 5:  ret_val = 1; break;
            case 6:  ret_val = 256; break;
            default: ret_val = 0; break;
        }
    }
    else if (name == "s3eSoundSetInt") {
        if (r0 == 0) master_volume_ = std::clamp<int32_t>(static_cast<int32_t>(r1), 0, 256);
        ret_val = 0;
    }
    else if (name == "s3eSoundGetFreeChannel") {
        ret_val = static_cast<uint32_t>(-1);
        for (int i = 0; i < MAX_SOUND_CHANNELS; ++i)
            if (!channels_[i].playing) { ret_val = i; break; }
    }
    else if (name == "s3eSoundChannelRegister") {    // (channel, cbid, fn, userdata)
        if (r0 < MAX_SOUND_CHANNELS && r1 < 4) { channels_[r0].cb_fn[r1] = r2; channels_[r0].cb_ud[r1] = r3; }
        ret_val = 0;
    }
    else if (name == "s3eSoundChannelUnRegister") {  // (channel, cbid)
        if (r0 < MAX_SOUND_CHANNELS && r1 < 4) { channels_[r0].cb_fn[r1] = 0; channels_[r0].cb_ud[r1] = 0; }
        ret_val = 0;
    }
    else if (name == "s3eSoundChannelSetInt") {      // (channel, prop, value)
        if (r0 < MAX_SOUND_CHANNELS) {
            SoundChannel& c = channels_[r0];
            if (r1 == 1) c.rate = r2;
            else if (r1 == 2) c.uservar = r2;
            else if (r1 == 3) c.volume = std::clamp<int32_t>(static_cast<int32_t>(r2), 0, 256);
            else if (r1 == 4) c.paused = r2 != 0;
        }
        ret_val = 0;
    }
    else if (name == "s3eSoundChannelGetInt") {      // (channel, prop)
        ret_val = 0;
        if (r0 < MAX_SOUND_CHANNELS) {
            const SoundChannel& c = channels_[r0];
            if (r1 == 0) ret_val = c.playing ? 1 : 0;
            else if (r1 == 1) ret_val = c.rate;
            else if (r1 == 2) ret_val = c.uservar;
            else if (r1 == 3) ret_val = static_cast<uint32_t>(c.volume);
            else if (r1 == 4) ret_val = c.paused ? 1 : 0;
        }
    }
    else if (name == "s3eSoundChannelPlay") {        // (channel, int16* start, numSamples, repeat, loopfrom)
        if (r0 < MAX_SOUND_CHANNELS) {
            uint32_t sp = 0, loop_from = 0;
            uc->RegRead(Cpu::REG_SP, &sp);
            uc->MemRead(sp, &loop_from, 4);
            SoundChannel& c = channels_[r0];
            c.start = r1; c.num_samples = r2; c.repeat = static_cast<int32_t>(r3);
            c.loop_from = static_cast<int32_t>(loop_from); c.pos = 0;
            c.playing = true; c.paused = false;
            if (!sound_started_) sound_started_ = AudioOut::Instance().Start(SOUND_RATE);
        }
        ret_val = 0;
    }
    else if (name == "s3eSoundChannelStop") {
        if (r0 < MAX_SOUND_CHANNELS) channels_[r0].playing = channels_[r0].paused = false;
        ret_val = 0;
    }
    else if (name == "s3eSoundChannelPause") {
        if (r0 < MAX_SOUND_CHANNELS) channels_[r0].paused = true;
        ret_val = 0;
    }
    else if (name == "s3eSoundChannelResume") {
        if (r0 < MAX_SOUND_CHANNELS) channels_[r0].paused = false;
        ret_val = 0;
    }
    else if (name == "s3eSoundStopAllChannels") {
        for (auto& c : channels_) c.playing = c.paused = false;
        ret_val = 0;
    }
    else if (name == "s3eAudioGetInt") {
        // r0 = property
        switch (r0) {
            case 0:  ret_val = 256; break; // S3E_AUDIO_VOLUME
            case 1:  ret_val = audio_.playing ? 1 : 0; break; // S3E_AUDIO_STATUS (1=playing, 2=paused, 0=stopped)
            case 5:  ret_val = 1; break;   // S3E_AUDIO_NUM_CHANNELS (or available)
            default: ret_val = 0; break;
        }
    }
    else if (name == "s3eAudioSetInt") {
        // (prop, val)
        if (r0 == 0) audio_.volume = r1;
        ret_val = 0;
    }
    else if (name == "s3eAudioIsPlaying") {
        ret_val = audio_.playing ? 1 : 0;
    }
    else if (name == "s3eAudioPlayFromBuffer") {
        // (buffer, len, repeat)
        audio_.playing = true;
        audio_.paused = false;
        ret_val = 0;
    }
    else if (name == "s3eAudioStop") {
        audio_.playing = false;
        audio_.paused = false;
        ret_val = 0;
    }
    else if (name == "s3eAudioPause") {
        audio_.paused = true;
        ret_val = 0;
    }
    else if (name == "s3eAudioResume") {
        audio_.paused = false;
        ret_val = 0;
    }
    else if (name == "s3eAudioRegister") {
        // (event, fn, userdata)
        audio_.cb_fn = r1;
        audio_.cb_ud = r2;
        ret_val = 0;
    }
    else if (name == "s3eDebugTraceLine") {
        std::string msg = ReadGuestString(uc, r0);
        std::cout << "[Guest] " << msg << std::endl;
        ret_val = 0;
    }
    else if (name == "s3eDebugAssertShow") {
        // s3eErrorShowResult s3eDebugAssertShow(s3eMessageType type, const char* string)
        std::cout << "[GuestAssert] " << ReadGuestString(uc, r1, 512) << " (LR=0x" << std::hex << lr << std::dec << ")" << std::endl;
        ret_val = 0; // S3E_ERROR_SHOW_CONTINUE
    }
    else if (name == "s3eDebugErrorShow") { std::cout << "[GuestError] " << ReadGuestString(uc, r1) << std::endl;
        ret_val = 0;
    }

    // Set return value in R0
    if (regfile) regfile[0] = ret_val; else uc->RegWrite(Cpu::REG_R0, &ret_val);
    g_recent[g_recent_n++ % 64] = { &name, lr, r0, r1, r2, r3, ret_val };
    if (!g_traced_handles.empty() && name.compare(0, 7, "s3eFile") == 0) {
        uint32_t h = (name == "s3eFileRead" || name == "s3eFileWrite") ? r3 : r0;
        if (g_traced_handles.count(h)) {
            std::cout << "[FTrace] " << name << std::hex << " lr=0x" << lr << " r0=0x" << r0 << " r1=0x" << r1
                      << " r2=0x" << r2 << " -> 0x" << ret_val;
            if (name == "s3eFileRead" && ret_val) {
                uint32_t n = (std::min<uint32_t>)(ret_val * r1, 16u); uint8_t b[16];
                uc->MemRead(r0, b, n);
                std::cout << " [";
                for (uint32_t i = 0; i < n; ++i) std::cout << (i ? " " : "") << (b[i] < 16 ? "0" : "") << uint32_t(b[i]);
                std::cout << "]";
            }
            if (name != "s3eFileClose") std::cout << " pos=0x" << VirtualFileSystem::Instance().Tell(h);
            { // rough backtrace: Thumb return addresses found on the stack
                uint32_t sp = 0, words[96] = {};
                uc->RegRead(Cpu::REG_SP, &sp);
                uc->MemRead(sp, words, sizeof(words));
                std::cout << " bt:";
                for (uint32_t w : words) if ((w & 1) && w >= G(0x4A000000) && w < G(0x4A0F2000)) std::cout << " " << w;
            }
            std::cout << std::dec << std::endl;
            if (name == "s3eFileClose") g_traced_handles.erase(h);
        }
    }

    static unsigned timer_skip = 0;   // (the clock costs a 64-bit division on the 3DS: every 8th syscall is plenty)
    if (timer_.active && (++timer_skip & 7) == 0) {
        auto now = static_cast<uint32_t>(GameMs());
        if (now >= timer_.due) {
            timer_.active = false;
            pending_cbs_.push_back({timer_.fn, 0, timer_.ud});
        }
    }
    if (!in_cb_) PumpAudio(uc);
    UpdateFocusPause();

    // Run one queued guest callback as if the syscall had called it; the return stub restores state.
    if (!in_cb_ && !pending_cbs_.empty()) {
        saved_[0] = ret_val; saved_[1] = r1; saved_[2] = r2; saved_[3] = r3; saved_[5] = lr;
        uc->RegRead(Cpu::REG_R12, &saved_[4]);
        if (DispatchPending(uc)) return;
    }

    // Restore PC to LR and restore CPSR Thumb bit if LR bit 0 was set
    uint32_t target_pc = lr & ~1;
    uc->RegWrite(Cpu::REG_PC, &target_pc);
    if (lr & 1) {
        uint32_t cpsr = 0;
        uc->RegRead(Cpu::REG_CPSR, &cpsr);
        cpsr |= (1 << 5); // Thumb bit
        uc->RegWrite(Cpu::REG_CPSR, &cpsr);
    }
}


// ---- guest callbacks -------------------------------------------------------------------------------

// Pause on focus loss: the game's PAUSE callback (in gameplay it opens the pause menu), then the game freezes
// in GLES::Present until focus returns; UNPAUSE when it is resumed. Callbacks take (systemData, userData).
void S3ERuntime::UpdateFocusPause() {
    const bool want = pause_on_focus_loss_ && focus_lost_ && !quit_requested_;
    if (want == focus_pause_sent_) return;
    focus_pause_sent_ = want;
    const auto it = device_cbs_.find(want ? 0 : 1);   // S3E_DEVICE_PAUSE / S3E_DEVICE_UNPAUSE
    if (VerboseLog()) std::cout << "[Device] Focus " << (want ? "lost: PAUSE" : "back: UNPAUSE") << (it == device_cbs_.end() ? " (no callback)" : "") << std::endl;
    if (want) {
        if (it == device_cbs_.end()) { focus_paused_ = true; return; }
        pending_cbs_.push_back({it->second.first, 0, it->second.second, nullptr, [this](uint32_t) { if (focus_pause_sent_) focus_paused_ = true; }});
    } else {
        focus_paused_ = false;
        if (it != device_cbs_.end()) pending_cbs_.push_back({it->second.first, 0, it->second.second});
    }
}

// Starts the next queued guest callback (saved_ must hold the interrupted syscall's return state).
bool S3ERuntime::DispatchPending(Cpu* uc) {
    while (!pending_cbs_.empty()) {
        PendingCallback cb = std::move(pending_cbs_.front());
        pending_cbs_.pop_front();
        if (cb.prepare && !cb.prepare(cb)) {
            if (cb.on_done) cb.on_done(CB_SKIPPED);   // may queue more
            continue;
        }
        in_cb_ = true;
        running_cb_done_ = std::move(cb.on_done);
        uint32_t ret_tramp = TRAMPOLINE_BASE + static_cast<uint32_t>(imports_.size()) * 8;
        uint32_t pc = cb.fn & ~1u, cpsr = 0;
        uc->RegWrite(Cpu::REG_R0, &cb.arg0);
        uc->RegWrite(Cpu::REG_R1, &cb.arg1);
        uc->RegWrite(Cpu::REG_R2, &cb.arg2);
        uc->RegWrite(Cpu::REG_R3, &cb.arg3);
        uc->RegWrite(Cpu::REG_LR, &ret_tramp);
        uc->RegWrite(Cpu::REG_PC, &pc);         // PC first: the write resets the Thumb state...
        uc->RegRead(Cpu::REG_CPSR, &cpsr);
        cpsr = (cb.fn & 1) ? (cpsr | 0x20) : (cpsr & ~0x20u);
        uc->RegWrite(Cpu::REG_CPSR, &cpsr);     // ...then set it for the callback
        return true;
    }
    return false;
}

// ---- s3eSound mixer ----------------------------------------------------------------------------------

// Called on the emu thread after every syscall (outside callbacks). When the output ring is low, mixes
// one SOUND_BLOCK: raw-PCM channels on the host, GEN_AUDIO channels by calling their guest generators
// back-to-back into one guest buffer (m_Mix = 1), then hands the block to AudioOut.
void S3ERuntime::PumpAudio(Cpu* uc) {
    if (!sound_started_ || mix_active_) return;
    // Called after every syscall (~2000 a frame); asking the output how much is queued means a lock, and a block
    // lasts 23 ms, so look only every 32 calls.
    static unsigned skip = 0;
    if (++skip & 31) return;
    if (AudioOut::Instance().Queued() >= static_cast<size_t>(SOUND_BLOCK * 3)) return;
    if (!g_mix_buf_) {
        g_mix_buf_ = GuestAlloc(SOUND_BLOCK * 2);
        g_gen_info_ = GuestAlloc(0x40);
        g_end_info_ = GuestAlloc(8 * MAX_SOUND_CHANNELS);
    }
    mix_active_ = true;
    mix_acc_.assign(SOUND_BLOCK, 0);
    ZeroGuest(uc, g_mix_buf_, SOUND_BLOCK * 2);
    std::vector<int> gen;
    for (int ch = 0; ch < MAX_SOUND_CHANNELS; ++ch) {
        const SoundChannel& c = channels_[ch];
        if (!c.playing || c.paused) continue;
        if (c.cb_fn[1]) gen.push_back(ch);
        else MixRawChannel(uc, ch);
    }
    QueueGen(uc, std::move(gen), 0);
}

void S3ERuntime::QueueGen(Cpu* uc, std::vector<int> chans, size_t i) {
    if (i >= chans.size()) { FinishMixBlock(uc); return; }
    const int ch = chans[i];
    PendingCallback cb{};
    // Fill s3eSoundGenAudioInfo right before the call; skip channels stopped since the block started.
    cb.prepare = [this, uc, ch](PendingCallback& p) {
        const SoundChannel& c = channels_[ch];
        if (!c.playing || c.paused || !c.cb_fn[1]) return false;
        const uint32_t info[9] = {
            static_cast<uint32_t>(ch), g_mix_buf_, static_cast<uint32_t>(SOUND_BLOCK), 1u,   // channel, target, samples, mix
            c.start, c.num_samples, static_cast<uint32_t>(c.repeat), 0u, 0u };               // orig start/len/repeat, end, stereo
        uc->MemWrite(g_gen_info_, info, sizeof(info));
        p.fn = c.cb_fn[1]; p.arg0 = g_gen_info_; p.arg1 = c.cb_ud[1];
        return true;
    };
    cb.on_done = [this, uc, chans = std::move(chans), i, ch](uint32_t ret) {
        if (ret != CB_SKIPPED) {
            uint32_t end = 0;
            uc->MemRead(g_gen_info_ + 0x1C, &end, 4);   // m_EndSample, set by the generator when the sound is over
            if (end) ChannelEnded(uc, ch);
        }
        QueueGen(uc, chans, i + 1);
    };
    pending_cbs_.push_back(std::move(cb));
}

void S3ERuntime::ChannelEnded(Cpu* uc, int ch) {
    SoundChannel& c = channels_[ch];
    c.playing = false;
    if (c.cb_fn[0]) {                                     // END_SAMPLE: {channel, repeatsRemaining = 0}
        const uint32_t info[2] = { static_cast<uint32_t>(ch), 0u };
        uc->MemWrite(g_end_info_ + 8 * ch, info, sizeof(info));
        pending_cbs_.push_back({ c.cb_fn[0], g_end_info_ + 8 * ch, c.cb_ud[0] });
    }
}

// Raw 16-bit PCM channel (no GEN_AUDIO callback): resample from the channel rate, apply its volume.
void S3ERuntime::MixRawChannel(Cpu* uc, int ch) {
    SoundChannel& c = channels_[ch];
    if (!c.start || !c.num_samples) { ChannelEnded(uc, ch); return; }
    std::vector<int16_t> src(c.num_samples);
    if (uc->MemRead(c.start, src.data(), src.size() * 2) != Cpu::OK) { ChannelEnded(uc, ch); return; }
    const double step = static_cast<double>(c.rate) / SOUND_RATE;
    for (int i = 0; i < SOUND_BLOCK; ++i) {
        if (c.pos >= c.num_samples) {
            if (c.repeat == 1) { ChannelEnded(uc, ch); return; }   // 0 = loop forever, n = play n times
            if (c.repeat > 1) --c.repeat;
            c.pos = std::min<double>(c.loop_from, c.num_samples - 1);
        }
        mix_acc_[i] += src[static_cast<size_t>(c.pos)] * c.volume / 256;
        c.pos += step;
    }
}

void S3ERuntime::FinishMixBlock(Cpu* uc) {
    std::vector<int16_t> block(SOUND_BLOCK);
    uc->MemRead(g_mix_buf_, block.data(), SOUND_BLOCK * 2);
    for (int i = 0; i < SOUND_BLOCK; ++i) {
        int32_t v = (static_cast<int32_t>(block[i]) + mix_acc_[i]) * master_volume_ / 256;
        block[i] = static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
    }
    AudioOut::Instance().Push(block.data(), block.size());
    static FILE* dump = [] { const char* p = getenv("MGS_SOUND_DUMP"); return p ? fopen(p, "wb") : nullptr; }();
    if (dump) { fwrite(block.data(), 2, block.size(), dump); fflush(dump); }   // raw s16le mono @ SOUND_RATE
    mix_active_ = false;
}
