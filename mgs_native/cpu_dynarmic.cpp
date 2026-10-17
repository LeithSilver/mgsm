// Dynarmic CPU backend (README.md §6.27): the game's ARMv5 / Thumb code recompiled by Dynarmic (A32 front end) on
// an x86-64 or arm64 host. The Switch's route: Unicorn's QEMU can't go there, Dynarmic's arm64 back end can.
//
// Memory: guest regions are host allocations listed in a 4 KB page table that Dynarmic reads inline; MemRead /
// MemWrite go through the same table. Import trampolines keep Unicorn's `SVC #index; BX LR` (CallSVC with PC =
// trampoline + 4). Code hooks change no code: at a hooked address PreCodeReadHook ends the block with a supervisor
// call (kHookSvc) instead of the instruction; CallSVC then runs the hooks and the instruction itself through the
// shared interpreter (guest_interp.cpp), and execution goes on after it. So any address can be hooked, even inside
// a 4-byte function.
#include "cpu.h"

#ifdef MGS_DYNARMIC
#include "guest_interp.h"
#include "loader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/ir/terminal.h>

namespace {
constexpr uint32_t kPageBits = 12, kPageSize = 1u << kPageBits;
constexpr uint32_t kHookSvc = 0xFFFFFF;   // our supervisor call at a hooked address (imports use their index)
constexpr uint32_t T_BIT = 0x20;

class DynarmicCpu final : public Cpu, private Dynarmic::A32::UserCallbacks {
public:
    DynarmicCpu() : pages_(new std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>{}) {
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = this;
        cfg.page_table = pages_.get();
        cfg.arch_version = Dynarmic::A32::ArchVersion::v6K;   // the N-Gage 2.0 phones' ARM11
        cfg.enable_cycle_counting = false;
        cfg.define_unpredictable_behaviour = true;
        jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
    }

    int MemMap(uint64_t addr, size_t size) override {
        if ((addr | size) & (kPageSize - 1) || addr + size > (1ull << 32)) return 1;
        auto block = std::make_unique<std::uint8_t[]>(size);   // zeroed
        for (uint64_t a = addr; a < addr + size; a += kPageSize) {
            if ((*pages_)[a >> kPageBits]) return 1;   // overlaps a mapping
        }
        for (uint64_t a = addr; a < addr + size; a += kPageSize) (*pages_)[a >> kPageBits] = block.get() + (a - addr);
        blocks_.push_back(std::move(block));
        return OK;
    }
    int MemRead(uint64_t addr, void* out, size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        while (size) {
            std::uint8_t* p = Host(static_cast<uint32_t>(addr));
            if (!p) return ERR_READ_UNMAPPED;
            const size_t n = (std::min)(size, static_cast<size_t>(kPageSize - (addr & (kPageSize - 1))));
            memcpy(o, p, n);
            o += n; addr += n; size -= n;
        }
        return OK;
    }
    int MemWrite(uint64_t addr, const void* data, size_t size) override {
        const uint64_t start = addr;
        const size_t total = size;
        auto* d = static_cast<const std::uint8_t*>(data);
        while (size) {
            std::uint8_t* p = Host(static_cast<uint32_t>(addr));
            if (!p) return ERR_WRITE_UNMAPPED;
            const size_t n = (std::min)(size, static_cast<size_t>(kPageSize - (addr & (kPageSize - 1))));
            memcpy(p, d, n);
            d += n; addr += n; size -= n;
        }
        // Code changed (the runtime's stubs and patches): drop translations of it
        if (start < G(0x4A0E3000) && start + total > TRAMPOLINE_BASE) jit_->InvalidateCacheRange(static_cast<uint32_t>(start), total);
        return OK;
    }
    int RegRead(int reg, void* out) override {
        uint32_t v;
        if (reg >= REG_R0 && reg <= REG_PC) v = jit_->Regs()[reg];
        else if (reg == REG_CPSR) v = jit_->Cpsr();
        else return 1;
        memcpy(out, &v, 4);
        return OK;
    }
    int RegWrite(int reg, const void* value) override {
        uint32_t v;
        memcpy(&v, value, 4);
        if (reg == REG_PC) {   // like Unicorn: bit 0 of a PC write selects Thumb
            jit_->Regs()[15] = v & ~1u;
            if (v & 1) jit_->SetCpsr(jit_->Cpsr() | T_BIT);
        } else if (reg >= REG_R0 && reg < REG_PC) jit_->Regs()[reg] = v;
        else if (reg == REG_CPSR) jit_->SetCpsr(v);
        else return 1;
        return OK;
    }

    int AddCodeHook(CodeHook fn, void* user, uint64_t begin, uint64_t end) override {
        if (end != begin) { std::cerr << "[Dynarmic] Code hook ranges aren't supported" << std::endl; return 1; }
        hooks_[static_cast<uint32_t>(begin)].push_back({fn, user});
        jit_->InvalidateCacheRange(static_cast<uint32_t>(begin), 4);
        return OK;
    }
    int AddSvcHook(SvcHook fn, void* user) override { svc_ = fn; svc_user_ = user; return OK; }
    int AddWriteHook(WriteHook, void*, uint64_t, uint64_t) override { return 1; }   // debug only (MGS_WATCH)

    int Start(uint64_t pc, uint64_t until, size_t) override {
        until_ = static_cast<uint32_t>(until) & ~1u;
        jit_->Regs()[15] = static_cast<uint32_t>(pc) & ~1u;
        jit_->SetCpsr(0x10 | ((pc & 1) ? T_BIT : 0));
        done_ = false;
        error_ = OK;
        while (!stop_ && !done_ && error_ == OK) jit_->Run();
        return error_;
    }
    int Stop() override { stop_ = true; jit_->HaltExecution(); return OK; }
    const char* ErrorText(int err) const override {
        switch (err) {
            case OK: return "OK";
            case ERR_READ_UNMAPPED: return "read from unmapped memory";
            case ERR_WRITE_UNMAPPED: return "write to unmapped memory";
            default: return "Dynarmic error (see the log)";
        }
    }

private:
    struct Hook { CodeHook fn; void* user; };

    std::uint8_t* Host(uint32_t a) const {
        std::uint8_t* page = (*pages_)[a >> kPageBits];
        return page ? page + (a & (kPageSize - 1)) : nullptr;
    }
    template <typename T> T Load(uint32_t a) {
        T v{};
        if (MemRead(a, &v, sizeof(T)) != OK) Fail(ERR_READ_UNMAPPED, a);
        return v;
    }
    template <typename T> void Store(uint32_t a, T v) {
        if (MemWrite(a, &v, sizeof(T)) != OK) Fail(ERR_WRITE_UNMAPPED, a);
    }
    void Fail(int err, uint32_t addr) {
        if (error_ == OK) {
            error_ = err;
            std::cerr << "[Dynarmic] " << ErrorText(err) << " at 0x" << std::hex << addr << ", pc 0x" << jit_->Regs()[15] << std::dec << std::endl;
        }
        jit_->HaltExecution();
    }

    // ---- Dynarmic callbacks (page-table misses, supervisor calls, exceptions) ----
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override {
        if (!Host(vaddr)) return std::nullopt;
        uint32_t v = 0;
        MemRead(vaddr, &v, 4);
        return v;
    }
    bool PreCodeReadHook(bool, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) override {
        if (!hooks_.count(pc)) return true;
        ir.BranchWritePC(ir.Imm32(pc));
        ir.CallSupervisor(ir.Imm32(kHookSvc));
        ir.SetTerm(Dynarmic::IR::Term::ReturnToDispatch{});
        return false;
    }
    std::uint8_t MemoryRead8(std::uint32_t a) override { return Load<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(std::uint32_t a) override { return Load<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(std::uint32_t a) override { return Load<std::uint32_t>(a); }
    std::uint64_t MemoryRead64(std::uint32_t a) override { return Load<std::uint64_t>(a); }
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { Store(a, v); }
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { Store(a, v); }
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { Store(a, v); }
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { Store(a, v); }

    void CallSVC(std::uint32_t swi) override {
        if (swi != kHookSvc) {   // an import trampoline: PC = trampoline + 4, as under Unicorn
            if (svc_) svc_(this, swi, svc_user_);
            return;
        }
        // A hooked address: the hooks, then the instruction itself unless a hook moved the PC
        const uint32_t pc = jit_->Regs()[15], cpsr = jit_->Cpsr();
        auto it = hooks_.find(pc);
        if (it != hooks_.end()) for (const Hook& h : it->second) h.fn(this, pc, 2, h.user);
        if (jit_->Regs()[15] != pc || ((jit_->Cpsr() ^ cpsr) & T_BIT)) return;
        uint32_t regs[16], ps = jit_->Cpsr();
        memcpy(regs, jit_->Regs().data(), sizeof(regs));
        struct Interp : GuestInterp {
            Interp(DynarmicCpu* c, uint32_t* r, uint32_t* p) : GuestInterp(r, p), c_(c) {}
            uint16_t Fetch16(uint32_t a) override { return c_->Load<std::uint16_t>(a); }
            uint32_t Read32(uint32_t a) override { return c_->Load<std::uint32_t>(a); }
            uint16_t Read16(uint32_t a) override { return c_->Load<std::uint16_t>(a); }
            uint8_t Read8(uint32_t a) override { return c_->Load<std::uint8_t>(a); }
            void Write32(uint32_t a, uint32_t v) override { c_->Store(a, v); }
            void Write16(uint32_t a, uint16_t v) override { c_->Store(a, v); }
            void Write8(uint32_t a, uint8_t v) override { c_->Store(a, v); }
            DynarmicCpu* c_;
        } interp(this, regs, &ps);
        if (!interp.Step()) { Fail(1, pc); return; }
        memcpy(jit_->Regs().data(), regs, sizeof(regs));
        jit_->SetCpsr(ps);
    }
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override {
        if ((pc & ~1u) == until_) { done_ = true; jit_->HaltExecution(); return; }   // returned from the entry point
        if (e == Dynarmic::A32::Exception::PreloadData || e == Dynarmic::A32::Exception::PreloadDataWithIntentToWrite ||
            e == Dynarmic::A32::Exception::PreloadInstruction || e == Dynarmic::A32::Exception::Yield) return;   // hints
        std::cerr << "[Dynarmic] Exception " << static_cast<int>(e) << " at pc 0x" << std::hex << pc << std::dec << std::endl;
        Fail(1, pc);
    }
    void InterpreterFallback(std::uint32_t pc, size_t n) override {
        std::cerr << "[Dynarmic] No translation for " << n << " instruction(s) at 0x" << std::hex << pc << std::dec << std::endl;
        Fail(1, pc);
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1u << 20; }

    std::unique_ptr<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> pages_;
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    std::map<uint32_t, std::vector<Hook>> hooks_;
    SvcHook svc_ = nullptr;
    void* svc_user_ = nullptr;
    uint32_t until_ = 0;
    std::atomic<bool> stop_{false};
    bool done_ = false;
    int error_ = OK;
};
}

std::unique_ptr<Cpu> Cpu::CreateDynarmic(std::string*) { return std::make_unique<DynarmicCpu>(); }

#else
std::unique_ptr<Cpu> Cpu::CreateDynarmic(std::string* error) {
    if (error) *error = "this build has no Dynarmic (configure with MGS_DYNARMIC_DIR)";
    return nullptr;
}
#endif
