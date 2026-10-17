#pragma once

#include <cstdint>

// A small interpreter for single guest instructions (README.md §6.26 / §6.27): the Thumb-1 and ARM forms found at
// the code-hook sites (tools/native_hooks.py lists them). The native and Dynarmic backends can't stop before an
// instruction the way Unicorn does, so they run the hooked instruction(s) through this after calling the hooks.
// Registers and memory come from the backend: r[16] and the CPSR (bit 5 = Thumb) are read and written in place.
class GuestInterp {
public:
    GuestInterp(uint32_t* regs, uint32_t* cpsr) : r_(regs), cpsr_(cpsr) {}
    virtual ~GuestInterp() = default;

    // Runs the instruction at r[15]. False (with a log line) for an instruction it doesn't know.
    bool Step();

protected:
    virtual uint16_t Fetch16(uint32_t a) = 0;   // instruction halfwords (a backend may serve unpatched code)
    virtual uint32_t Read32(uint32_t a) = 0;
    virtual uint16_t Read16(uint32_t a) = 0;
    virtual uint8_t Read8(uint32_t a) = 0;
    virtual void Write32(uint32_t a, uint32_t v) = 0;
    virtual void Write16(uint32_t a, uint16_t v) = 0;
    virtual void Write8(uint32_t a, uint8_t v) = 0;

private:
    bool StepThumb();
    bool StepArm();
    void SetNZ(uint32_t v);
    uint32_t AddFlags(uint32_t a, uint32_t b, uint32_t carry);
    bool Cond(uint32_t cond) const;
    bool Unsupported(uint32_t insn, bool thumb);

    uint32_t* r_;
    uint32_t* cpsr_;
};
