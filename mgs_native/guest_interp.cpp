#include "guest_interp.h"

#include <iostream>

namespace {
constexpr uint32_t T_BIT = 0x20;
}

bool GuestInterp::Step() { return (*cpsr_ & T_BIT) ? StepThumb() : StepArm(); }

void GuestInterp::SetNZ(uint32_t v) {
    *cpsr_ = (*cpsr_ & ~0xC0000000u) | (v & 0x80000000u) | (v == 0 ? 0x40000000u : 0);
}

uint32_t GuestInterp::AddFlags(uint32_t a, uint32_t b, uint32_t carry) {
    const uint64_t u = static_cast<uint64_t>(a) + b + carry;
    const uint32_t r = static_cast<uint32_t>(u);
    uint32_t& f = *cpsr_;
    f &= ~0xF0000000u;
    f |= (r & 0x80000000u) | (r == 0 ? 0x40000000u : 0) | ((u >> 32) ? 0x20000000u : 0) |
         ((~(a ^ b) & (a ^ r) & 0x80000000u) ? 0x10000000u : 0);
    return r;
}

bool GuestInterp::Cond(uint32_t cond) const {
    const uint32_t f = *cpsr_;
    const bool N = f >> 31 & 1, Z = f >> 30 & 1, C = f >> 29 & 1, V = f >> 28 & 1;
    switch (cond) {
        case 0: return Z; case 1: return !Z; case 2: return C; case 3: return !C; case 4: return N; case 5: return !N;
        case 6: return V; case 7: return !V; case 8: return C && !Z; case 9: return !C || Z; case 10: return N == V;
        case 11: return N != V; case 12: return !Z && N == V; case 13: return Z || N != V; default: return true;
    }
}

bool GuestInterp::Unsupported(uint32_t insn, bool thumb) {
    std::cerr << "[Interp] Unsupported " << (thumb ? "Thumb" : "ARM") << " instruction 0x" << std::hex << insn
              << " at 0x" << r_[15] << std::dec << std::endl;
    return false;
}

bool GuestInterp::StepThumb() {
    uint32_t* r = r_;
    uint32_t& cpsr = *cpsr_;
    const uint32_t pc = r[15], i = Fetch16(pc);
    uint32_t next = pc + 2;
    const uint32_t rd = i & 7, rs = (i >> 3) & 7, imm5 = (i >> 6) & 31, rn8 = (i >> 8) & 7, imm8 = i & 0xFF;
    if (i < 0x1800) {   // LSL / LSR / ASR immediate (LSL #0 = MOVS)
        const uint32_t op = i >> 11, v = r[rs];
        uint32_t res = v, carry = (cpsr >> 29) & 1;
        if (op == 0) { if (imm5) { carry = (v >> (32 - imm5)) & 1; res = v << imm5; } }
        else if (op == 1) { const uint32_t n = imm5 ? imm5 : 32; carry = (v >> (n - 1)) & 1; res = n == 32 ? 0 : v >> n; }
        else { const uint32_t n = imm5 ? imm5 : 32; carry = (v >> (n - 1)) & 1; res = n == 32 ? (static_cast<int32_t>(v) >> 31) : static_cast<uint32_t>(static_cast<int32_t>(v) >> n); }
        r[rd] = res;
        SetNZ(res);
        cpsr = (cpsr & ~0x20000000u) | (carry << 29);
    } else if (i < 0x2000) {   // ADDS / SUBS register or imm3
        const uint32_t operand = (i & 0x0400) ? ((i >> 6) & 7) : r[(i >> 6) & 7];
        r[rd] = (i & 0x0200) ? AddFlags(r[rs], ~operand, 1) : AddFlags(r[rs], operand, 0);
    } else if (i < 0x4000) {   // MOVS / CMP / ADDS / SUBS imm8
        switch ((i >> 11) & 3) {
            case 0: r[rn8] = imm8; SetNZ(imm8); break;
            case 1: AddFlags(r[rn8], ~imm8, 1); break;
            case 2: r[rn8] = AddFlags(r[rn8], imm8, 0); break;
            default: r[rn8] = AddFlags(r[rn8], ~imm8, 1); break;
        }
    } else if ((i & 0xFC00) == 0x4400) {   // hi-register ADD / CMP / MOV, BX / BLX
        const uint32_t op = (i >> 8) & 3, m = (i >> 3) & 15, d = (i & 7) | ((i >> 4) & 8);
        const uint32_t mv = m == 15 ? pc + 4 : r[m];
        if (op == 0) r[d] = r[d] + mv;
        else if (op == 1) AddFlags(r[d], ~mv, 1);
        else if (op == 2) { if (d == 15) { next = mv & ~1u; } else r[d] = mv; }
        else {
            if (i & 0x80) r[14] = next | 1;
            next = mv & ~1u;
            if (!(mv & 1)) cpsr &= ~T_BIT;
        }
    } else if ((i & 0xF800) == 0x4800) {   // LDR rd, [pc, #imm8*4]
        r[rn8] = Read32(((pc + 4) & ~3u) + imm8 * 4);
    } else if (i >= 0x6000 && i < 0x9000) {   // LDR/STR (B/H) [rn, #imm5]
        const uint32_t base = r[rs], kind = i >> 11;
        switch (kind) {
            case 0x0C: Write32(base + imm5 * 4, r[rd]); break;
            case 0x0D: r[rd] = Read32(base + imm5 * 4); break;
            case 0x0E: Write8(base + imm5, static_cast<uint8_t>(r[rd])); break;
            case 0x0F: r[rd] = Read8(base + imm5); break;
            case 0x10: Write16(base + imm5 * 2, static_cast<uint16_t>(r[rd])); break;
            default: r[rd] = Read16(base + imm5 * 2); break;
        }
    } else if ((i & 0xF000) == 0x9000) {   // STR / LDR [sp, #imm8*4]
        if (i & 0x0800) r[rn8] = Read32(r[13] + imm8 * 4); else Write32(r[13] + imm8 * 4, r[rn8]);
    } else if ((i & 0xF000) == 0xA000) {   // ADR / ADD rd, sp, #imm8*4
        r[rn8] = ((i & 0x0800) ? r[13] : ((pc + 4) & ~3u)) + imm8 * 4;
    } else if ((i & 0xFF00) == 0xB000) {   // ADD / SUB sp, #imm7*4
        const uint32_t v = (i & 0x7F) * 4;
        r[13] = (i & 0x80) ? r[13] - v : r[13] + v;
    } else if ((i & 0xFE00) == 0xB400) {   // PUSH {list, lr}
        uint32_t sp = r[13];
        if (i & 0x100) Write32(sp -= 4, r[14]);
        for (int k = 7; k >= 0; --k) if (i & (1u << k)) Write32(sp -= 4, r[k]);
        r[13] = sp;
    } else if ((i & 0xFE00) == 0xBC00) {   // POP {list, pc}
        uint32_t sp = r[13];
        for (int k = 0; k < 8; ++k) if (i & (1u << k)) { r[k] = Read32(sp); sp += 4; }
        if (i & 0x100) {
            const uint32_t v = Read32(sp);
            sp += 4;
            next = v & ~1u;
            if (!(v & 1)) cpsr &= ~T_BIT;
        }
        r[13] = sp;
    } else if ((i & 0xF000) == 0xD000 && (i & 0x0F00) < 0x0E00) {   // B<cond>
        if (Cond((i >> 8) & 15)) next = pc + 4 + (static_cast<int32_t>(static_cast<int8_t>(imm8)) * 2);
    } else if ((i & 0xF800) == 0xE000) {   // B
        next = pc + 4 + ((static_cast<int32_t>((i & 0x7FF) << 21) >> 21) * 2);
    } else if ((i & 0xF800) == 0xF000) {   // BL / BLX pair
        const uint32_t lo = Fetch16(pc + 2);
        const int32_t off = (static_cast<int32_t>((i & 0x7FF) << 21) >> 9) + static_cast<int32_t>((lo & 0x7FF) << 1);
        r[14] = (pc + 4) | 1;
        if ((lo & 0xF800) == 0xF800) next = pc + 4 + off;
        else if ((lo & 0xF800) == 0xE800) { next = (pc + 4 + off) & ~3u; cpsr &= ~T_BIT; }
        else return Unsupported(i | lo << 16, true);
    } else {
        return Unsupported(i, true);
    }
    r[15] = next;
    return true;
}

bool GuestInterp::StepArm() {
    uint32_t* r = r_;
    uint32_t& cpsr = *cpsr_;
    const uint32_t pc = r[15], i = Fetch16(pc) | (static_cast<uint32_t>(Fetch16(pc + 2)) << 16);
    uint32_t next = pc + 4;
    if (!Cond(i >> 28)) { r[15] = next; return true; }
    const uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15;
    if ((i & 0x0E000000) == 0x08000000) {   // LDM / STM
        const bool load = i & (1u << 20), wb = i & (1u << 21), up = i & (1u << 23), pre = i & (1u << 24);
        const uint32_t list = i & 0xFFFF;
        uint32_t n = 0;
        for (int k = 0; k < 16; ++k) n += (list >> k) & 1;
        const uint32_t base = r[rn];
        uint32_t a = up ? base : base - 4 * n;
        if (up == pre) a += 4;
        for (int k = 0; k < 16; ++k) {
            if (!(list & (1u << k))) continue;
            if (load) {
                const uint32_t v = Read32(a);
                if (k == 15) { next = v & ~1u; if (v & 1) cpsr |= T_BIT; } else r[k] = v;
            } else Write32(a, k == 15 ? pc + 8 : r[k]);
            a += 4;
        }
        if (wb) r[rn] = up ? base + 4 * n : base - 4 * n;
    } else if ((i & 0x0E000000) == 0x04000000) {   // LDR / STR (B) immediate
        const bool load = i & (1u << 20), byte = i & (1u << 22), wb = i & (1u << 21), up = i & (1u << 23), pre = i & (1u << 24);
        const uint32_t base = rn == 15 ? pc + 8 : r[rn], off = i & 0xFFF;
        const uint32_t a = pre ? (up ? base + off : base - off) : base;
        if (load) {
            const uint32_t v = byte ? Read8(a) : Read32(a);
            if (rd == 15) { next = v & ~1u; if (v & 1) cpsr |= T_BIT; } else r[rd] = v;
        } else if (byte) Write8(a, static_cast<uint8_t>(r[rd])); else Write32(a, r[rd]);
        if (!pre || wb) r[rn] = up ? base + off : base - off;
    } else {
        return Unsupported(i, false);
    }
    r[15] = next;
    return true;
}
