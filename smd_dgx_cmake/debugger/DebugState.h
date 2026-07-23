#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct M68kRegs {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc, sr, usp, isp;
};

struct Z80Regs {
    uint16_t af, bc, de, hl;
    uint16_t af2, bc2, de2, hl2;
    uint16_t ix, iy, sp, pc;
    uint8_t  i, r, im, iff1, iff2, halt;
};

// Byte-order contract for raw pointers below (LSB_FIRST build):
//   vram / sat : logical Genesis byte at address A lives at ptr[A ^ 1]
//                (the core stores VDP words as host little-endian).
//   cram / vsram: read per-entry as 16-bit LE (lo | hi<<8) — no extra XOR.
//   CRAM entry format: 0000 BBB0 GGG0 RRR0 (bits 1-3 R, 5-7 G, 9-11 B).
struct VdpState {
    uint8_t  reg[0x20];
    uint16_t status;
    uint32_t dma_len, dma_src;
    uint8_t  dma_type;     // 0=VRAM fill, 1=VRAM copy, 2=bus->VRAM/CRAM/VSRAM
    const uint8_t* vram;   // 0x10000 bytes – points into gpgx globals
    const uint8_t* cram;   // 0x80 bytes (64 colors)
    const uint8_t* vsram;  // 0x80 bytes (40 x 11-bit)
    const uint8_t* sat;    // 0x400 bytes – internal sprite attribute table copy
};

// Raw sound chip registers (shadow copies maintained by the core under HOOK_CPU).
struct SoundState {
    uint8_t fm[2][0x100];  // YM2612 raw registers: part I (ch 1-3, globals), part II (ch 4-6)
    int     psg[8];        // SN76489: [0,2,4]=tone period 0-2, [1,3,5]=attenuation 0-2,
                           //          [6]=noise control, [7]=noise attenuation
};

// A viewable/editable memory region (parity with the Gens hex editor).
// readRegion()/writeRegion() operate in LOGICAL byte order — the backend
// applies any host byte-swapping internally.
struct MemRegion {
    int         id;        // stable id, see GpgxBackend region table
    std::string name;      // "ROM", "RAM 68K", "RAM Z80", "VRAM", "CRAM", "VSRAM", "Regs ..."
    uint32_t    base;      // display base address (e.g. 0xFF0000 for 68K RAM)
    uint32_t    size;      // bytes
    bool        writable;
};

enum class BpType : uint8_t { PC = 1, Read = 2, Write = 3 };

struct Breakpoint {
    int      id      = 0;
    BpType   type    = BpType::PC;
    bool     is_vdp  = false;
    bool     enabled = true;
    uint32_t start   = 0;
    uint32_t end     = 0;
    std::string condition;
};
