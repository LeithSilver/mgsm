#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "cpu.h"

// The game is linked at STOCK_BASE; every address in the code and in README.md is a stock address. The image can
// be loaded elsewhere (its S3E relocation table moves every absolute pointer), so code that names a game address
// goes through G(): the address where that stock address is now. MGS_IMAGE_BASE=<hex> picks another base (a test
// for the native backends, which can't map 0x4A000000).
constexpr uint32_t STOCK_BASE      = 0x4A000000;
uint32_t ImageBase();
void SetImageBase(uint32_t base);
inline uint32_t G(uint32_t stock) { return stock - STOCK_BASE + ImageBase(); }
// Guest memory. Under Unicorn at these fixed addresses; the native backend puts each region wherever the host
// allocates it (SetupSandbox sets them). The trampolines sit right below the image (64 KB): Thumb BL reaches
// only +-4 MB.
extern uint32_t TRAMPOLINE_BASE, FB_BASE, STACK_BASE, HEAP_BASE;
constexpr uint32_t TRAMPOLINE_SIZE = 64 * 1024;
constexpr uint32_t FB_SIZE         = 1 * 1024 * 1024;     // 1 MB
constexpr uint32_t STACK_SIZE      = 2 * 1024 * 1024;     // 2 MB
// Guest heap: 128 MB under Unicorn, 64 MB natively (consoles; gameplay peaks at 34 MB), MGS_HEAP_MB=<n> overrides.
extern uint32_t HEAP_SIZE;

struct FixupEntry {
    uint32_t offset;
    uint16_t func_idx;
};

struct S3EBinary {
    uint32_t version;
    uint32_t base_addr;
    uint32_t entry_point;
    std::vector<std::string> imports;
    std::vector<uint8_t> code_and_data;
    std::vector<FixupEntry> arm_fixups;
    std::vector<FixupEntry> thumb_fixups;
    std::vector<uint32_t> relocations;   // image offsets of absolute pointers into the image (fixup block 1)
    std::string config;   // the embedded app.icf (S3E config text)
};

bool LoadS3E(const std::string& filepath, S3EBinary& out_bin);
// Maps and relocates the image at `base` (STOCK_BASE: as linked), the import trampolines, stack, framebuffer and heap.
// heap_size: 0 = the game's (128 MB, 64 natively, MGS_HEAP_MB); a small sandbox (the installer's) passes its own.
bool SetupSandbox(Cpu* uc, const S3EBinary& bin, uint32_t base = STOCK_BASE, uint32_t heap_size = 0);
