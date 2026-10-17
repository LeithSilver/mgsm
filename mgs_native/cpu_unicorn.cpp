// Cpu on Unicorn 2 (QEMU TCG): the only file that includes unicorn.h.
#include "cpu.h"

#include <unicorn/unicorn.h>
#include <vector>

namespace {

const int kRegs[] = {
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
    UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR };

class UnicornCpu : public Cpu {
public:
    explicit UnicornCpu(uc_engine* uc) : uc_(uc) {}
    ~UnicornCpu() override {
        uc_close(uc_);
        for (Thunk* t : thunks_) delete t;
    }

    int MemMap(uint64_t addr, size_t size) override { return uc_mem_map(uc_, addr, size, UC_PROT_ALL); }
    int MemRead(uint64_t addr, void* out, size_t size) override { return uc_mem_read(uc_, addr, out, size); }
    int MemWrite(uint64_t addr, const void* data, size_t size) override { return uc_mem_write(uc_, addr, data, size); }
    int RegRead(int reg, void* out) override { return Valid(reg) ? uc_reg_read(uc_, kRegs[reg], out) : UC_ERR_ARG; }
    int RegWrite(int reg, const void* value) override { return Valid(reg) ? uc_reg_write(uc_, kRegs[reg], value) : UC_ERR_ARG; }

    // Unicorn calls back with its own engine handle; a thunk per hook carries the Cpu and the user pointer.
    int AddCodeHook(CodeHook fn, void* user, uint64_t begin, uint64_t end) override {
        Thunk* t = NewThunk(reinterpret_cast<void*>(fn), user);
        uc_hook h;
        return uc_hook_add(uc_, &h, UC_HOOK_CODE, reinterpret_cast<void*>(+[](uc_engine*, uint64_t addr, uint32_t size, void* p) {
            const Thunk* t = static_cast<Thunk*>(p);
            reinterpret_cast<CodeHook>(t->fn)(t->cpu, addr, size, t->user);
        }), t, begin, end);
    }
    int AddSvcHook(SvcHook fn, void* user) override {
        Thunk* t = NewThunk(reinterpret_cast<void*>(fn), user);
        uc_hook h;
        return uc_hook_add(uc_, &h, UC_HOOK_INTR, reinterpret_cast<void*>(+[](uc_engine*, uint32_t intno, void* p) {
            const Thunk* t = static_cast<Thunk*>(p);
            reinterpret_cast<SvcHook>(t->fn)(t->cpu, intno, t->user);
        }), t, 1, 0);
    }
    int AddWriteHook(WriteHook fn, void* user, uint64_t begin, uint64_t end) override {
        Thunk* t = NewThunk(reinterpret_cast<void*>(fn), user);
        uc_hook h;
        return uc_hook_add(uc_, &h, UC_HOOK_MEM_WRITE, reinterpret_cast<void*>(+[](uc_engine*, uc_mem_type, uint64_t addr, int size, int64_t value, void* p) {
            const Thunk* t = static_cast<Thunk*>(p);
            reinterpret_cast<WriteHook>(t->fn)(t->cpu, addr, size, value, t->user);
        }), t, begin, end);
    }

    int Start(uint64_t pc, uint64_t until, size_t count) override { return uc_emu_start(uc_, pc, until, 0, count); }
    int Stop() override { return uc_emu_stop(uc_); }
    const char* ErrorText(int err) const override { return uc_strerror(static_cast<uc_err>(err)); }

private:
    struct Thunk { Cpu* cpu; void* fn; void* user; };
    static bool Valid(int reg) { return reg >= 0 && reg < static_cast<int>(sizeof(kRegs) / sizeof(kRegs[0])); }
    Thunk* NewThunk(void* fn, void* user) { thunks_.push_back(new Thunk{ this, fn, user }); return thunks_.back(); }

    uc_engine* uc_;
    std::vector<Thunk*> thunks_;
};

}

std::unique_ptr<Cpu> Cpu::CreateUnicorn(std::string* error) {
    uc_engine* uc = nullptr;
    const uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) {
        if (error) *error = uc_strerror(err);
        return nullptr;
    }
    return std::make_unique<UnicornCpu>(uc);
}
