#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// The guest ARM CPU (ARMv5 / Thumb, 32-bit), behind an interface so the runtime doesn't depend on one
// emulator. Today's backend is Unicorn (cpu_unicorn.cpp); a native backend (32-bit ARM hosts) or another
// recompiler can implement the same calls. The shape follows Unicorn's API on purpose (pointer outputs, error
// codes with Unicorn's values) so call sites read the same: uc_mem_read(uc, ...) became uc->MemRead(...).
// All calls happen on the thread running the guest (or before it starts), except Stop().
class Cpu {
public:
    // Registers (values are this interface's own, not Unicorn's)
    enum : int {
        REG_R0, REG_R1, REG_R2, REG_R3, REG_R4, REG_R5, REG_R6, REG_R7, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12,
        REG_SP, REG_LR, REG_PC, REG_CPSR
    };
    // Error codes (the values match Unicorn's uc_err)
    enum : int { OK = 0, ERR_READ_UNMAPPED = 6, ERR_WRITE_UNMAPPED = 7, ERR_GUEST_FAULT = 100 };   // (100: native backend, a signal in game code)

    // Hooks. addr = the instruction / write address; user = the pointer given when adding the hook.
    using CodeHook = void (*)(Cpu* cpu, uint64_t addr, uint32_t size, void* user);   // before the instruction at addr
    using SvcHook = void (*)(Cpu* cpu, uint32_t intno, void* user);                  // SVC (software interrupt)
    using WriteHook = void (*)(Cpu* cpu, uint64_t addr, int size, int64_t value, void* user);

    // The Unicorn backend in ARM mode (CPSR.T selects Thumb). Null with *error set on failure.
    static std::unique_ptr<Cpu> CreateUnicorn(std::string* error);
    // The native backend (cpu_native.cpp; 32-bit ARM hosts): the game's code runs on the host CPU. Null with
    // *error set where it isn't available.
    static std::unique_ptr<Cpu> CreateNative(std::string* error);
    // The Dynarmic backend (cpu_dynarmic.cpp; x86-64 / arm64 hosts, builds configured with MGS_DYNARMIC_DIR).
    static std::unique_ptr<Cpu> CreateDynarmic(std::string* error);
    virtual ~Cpu() = default;
    virtual bool IsNative() const { return false; }
    // r0..r15 as a plain array where the backend keeps them so (native): the runtime's per-syscall argument reads
    // and result write skip the virtual calls. Null otherwise. PC and CPSR writes still go through RegWrite.
    virtual uint32_t* RegisterFile() { return nullptr; }

    virtual int MemMap(uint64_t addr, size_t size) = 0;   // read/write/execute, at addr
    // Memory for the guest where the backend can put it: `preferred` (Unicorn) or wherever the host allocates
    // it (native). Read/write/execute, zeroed. Returns the address, 0 on failure.
    virtual uint64_t Reserve(uint64_t preferred, size_t size) { return MemMap(preferred, size) == OK ? preferred : 0; }
    // Import trampoline `index` (8 bytes at addr, ARM): entering it reaches the SVC hook with intno = index and
    // PC = addr + 4, as an `SVC #index; BX LR` pair does under Unicorn.
    virtual void WriteTrampoline(uint64_t addr, uint32_t index) {
        const uint32_t code[2] = { 0xEF000000 | index, 0xE12FFF1E };
        MemWrite(addr, code, sizeof(code));
    }
    virtual int MemRead(uint64_t addr, void* out, size_t size) = 0;
    virtual int MemWrite(uint64_t addr, const void* data, size_t size) = 0;
    virtual int RegRead(int reg, void* out) = 0;          // 32-bit value
    virtual int RegWrite(int reg, const void* value) = 0;

    // Code hooks fire for every instruction in [begin, end] (both inclusive; begin == end: one address).
    virtual int AddCodeHook(CodeHook fn, void* user, uint64_t begin, uint64_t end) = 0;
    virtual int AddSvcHook(SvcHook fn, void* user) = 0;
    virtual int AddWriteHook(WriteHook fn, void* user, uint64_t begin, uint64_t end) = 0;

    // Runs from pc (bit 0 = Thumb) until the PC reaches `until`, Stop() is called, `count` instructions ran
    // (0 = no limit), or an error. Returns OK or an error.
    virtual int Start(uint64_t pc, uint64_t until, size_t count = 0) = 0;
    virtual int Stop() = 0;   // any thread
    virtual const char* ErrorText(int err) const = 0;
};
