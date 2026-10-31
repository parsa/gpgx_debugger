#pragma once
//
// A Mega Drive ROM built by the test, not shipped with it.
//
// Tests must not depend on someone's game dump: it cannot be committed, and a
// test that silently skips when the file is missing is a test that never runs.
// gpgx picks the hardware from the file extension — ".bin" always means Mega
// Drive (core/loadrom.c) — so a handful of hand-assembled 68000 opcodes in a
// temp file is a complete, deterministic machine.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace synth {

// --- where the test code lives ---------------------------------------------
constexpr uint32_t kRomSize   = 0x8000;
constexpr uint32_t kEntry     = 0x0200;   // reset vector: Z80 bring-up, then the loop
constexpr uint32_t kJsr       = 0x0218;   // the jsr — where stepping tests start
constexpr uint32_t kAfterJsr  = 0x021E;   // where step-over must land
constexpr uint32_t kSubEntry  = 0x0300;   // nop; rts — the jsr target
constexpr uint32_t kSubRts    = 0x0302;
constexpr uint32_t kInitSp    = 0x00FFF000;

// Data-access routine, for read/write breakpoints. The longword write is the
// interesting one: it touches four bytes, so a breakpoint anywhere inside it
// has to fire — comparing only the start address misses every access that
// straddles the watched byte.
constexpr uint32_t kDataEntry = 0x0240;   // move.l #imm,(kDataAddr)
constexpr uint32_t kDataRead  = 0x024A;   // move.l (kDataAddr),d0
constexpr uint32_t kDataAddr  = 0x00FF4276;
constexpr uint32_t kDataValue = 0x12345678;

// Marker bytes at a known offset, so a test can prove the byte-order contract
// end to end rather than trusting a comment: the logical byte at A must come
// back from address A, whatever the host endianness does to the storage.
constexpr uint32_t kMarkerAddr = 0x0400;
inline const char* markerText() { return "SMD-DGX-BYTE-ORDER"; }

// CPU-state routines, reached by setting PC.
//   0500  4E72 2700            stop #$2700        ; all interrupts masked: stays stopped
//   0510  2E7C 00FF F001       movea.l #$FFF001,a7 ; odd stack pointer
//   0516  3038 0001            move.w ($0001).w,d0 ; address error -> stacking to an odd
//                                                  ; SP faults again -> the 68000 halts
constexpr uint32_t kStopEntry = 0x0500;
constexpr uint32_t kHaltEntry = 0x0510;

// Exception routines, reached by setting PC (SP must be a sane even address).
//   0520  4AFC                 illegal
//   0530  4E40                 trap #0
//   0540  3038 0001            move.w ($0001).w,d0 ; address error: odd word read
//   0600  4E71                 nop                ; every exception vector lands here
//   0602  60FE                 bra.s *
constexpr uint32_t kIllegalEntry = 0x0520;
constexpr uint32_t kTrapEntry    = 0x0530;
constexpr uint32_t kAddrErrEntry = 0x0540;
constexpr uint32_t kExcHandler   = 0x0600;

// Z80 bus wait, as a game does it on a Z80 still held in reset (power on):
//   0560  33FC 0000 00A11200   move.w #$0000,$A11200 ; assert Z80 /RESET
//   0568  33FC 0100 00A11100   move.w #$0100,$A11100 ; request the bus
//   0570  0839 0000 00A11100   btst   #0,$A11100     ; granted when D0 is 0
//   0578  66F6                 bne.s  $0570          ; never, while in reset
//   057A  4E75                 rts
constexpr uint32_t kZ80BusWait    = 0x0560;
constexpr uint32_t kZ80BusWaitRts = 0x057A;

inline void put32be(std::vector<uint8_t>& r, uint32_t at, uint32_t v)
{
    r[at + 0] = uint8_t(v >> 24); r[at + 1] = uint8_t(v >> 16);
    r[at + 2] = uint8_t(v >> 8);  r[at + 3] = uint8_t(v);
}

inline void put16be(std::vector<uint8_t>& r, uint32_t at, uint16_t v)
{
    r[at + 0] = uint8_t(v >> 8); r[at + 1] = uint8_t(v);
}

// 68000 program:
//   0200  33FC 0100 00A11100   move.w #$0100,$A11100   ; assert BUSREQ
//   0208  33FC 0100 00A11200   move.w #$0100,$A11200   ; release /RESET
//   0210  33FC 0000 00A11100   move.w #$0000,$A11100   ; release BUSREQ -> Z80 runs
//   0218  4EB9 0000 0300       jsr  $300
//   021E  4E71                 nop
//   0220  60F6                 bra.s $218
//   0300  4E71                 nop
//   0302  4E75                 rts
//
// The bring-up runs the Z80 the way a game does, so the Z80 tests exercise the
// real path instead of poking zstate. Sound RAM is zero at reset, which the Z80
// reads as a NOP sled — harmless until a test writes a program there.
//
// The tail is chosen so one program covers step-into (the jsr is followed),
// step-over (it is not), and the callstack (push at the jsr, pop at the rts).
inline std::vector<uint8_t> buildRom()
{
    std::vector<uint8_t> rom(kRomSize, 0xFF);

    // Vector table: only the two entries the reset path reads.
    put32be(rom, 0x00, kInitSp);
    put32be(rom, 0x04, kEntry);

    // Header. Not needed for detection, but a real one lets tests assert on
    // memory reads against a value a human can recognise in a hex dump.
    const char* hdr = "SEGA MEGA DRIVE ";
    for (int i = 0; i < 16; ++i) rom[0x100 + i] = uint8_t(hdr[i]);

    auto moveWordAbs = [&](uint32_t at, uint16_t imm, uint32_t dst) {
        put16be(rom, at + 0, 0x33FC);          // move.w #<data>,(xxx).L
        put16be(rom, at + 2, imm);
        put32be(rom, at + 4, dst);
    };
    moveWordAbs(kEntry + 0x00, 0x0100, 0x00A11100);
    moveWordAbs(kEntry + 0x08, 0x0100, 0x00A11200);
    moveWordAbs(kEntry + 0x10, 0x0000, 0x00A11100);

    put16be(rom, kJsr + 0, 0x4EB9);            // jsr $00000300
    put32be(rom, kJsr + 2, kSubEntry);
    put16be(rom, kAfterJsr, 0x4E71);           // nop
    put16be(rom, kAfterJsr + 2, 0x60F6);       // bra.s -10 -> kJsr

    put16be(rom, kSubEntry, 0x4E71);           // nop
    put16be(rom, kSubRts,   0x4E75);           // rts

    // 0240  23FC iiiiiiii aaaaaaaa   move.l #kDataValue,(kDataAddr).L
    // 024A  2039 aaaaaaaa            move.l (kDataAddr).L,d0
    // 0250  60FE                     bra.s *      (park here, never returns)
    put16be(rom, kDataEntry + 0, 0x23FC);
    put32be(rom, kDataEntry + 2, kDataValue);
    put32be(rom, kDataEntry + 6, kDataAddr);
    put16be(rom, kDataRead + 0, 0x2039);
    put32be(rom, kDataRead + 2, kDataAddr);
    put16be(rom, kDataRead + 6, 0x60FE);

    const char* m = markerText();
    for (uint32_t i = 0; m[i]; ++i) rom[kMarkerAddr + i] = uint8_t(m[i]);

    put16be(rom, kStopEntry + 0, 0x4E72);      // stop #$2700
    put16be(rom, kStopEntry + 2, 0x2700);
    put16be(rom, kHaltEntry + 0, 0x2E7C);      // movea.l #$00FFF001,a7
    put32be(rom, kHaltEntry + 2, 0x00FFF001);
    put16be(rom, kHaltEntry + 6, 0x3038);      // move.w ($0001).w,d0
    put16be(rom, kHaltEntry + 8, 0x0001);

    // Vectors 2..63 (faults, traps, interrupts) all park in one handler.
    for (uint32_t v = 2; v < 64; ++v) put32be(rom, v * 4, kExcHandler);
    put16be(rom, kExcHandler, 0x4E71);         // nop
    put16be(rom, kExcHandler + 2, 0x60FE);     // bra.s *
    put16be(rom, kIllegalEntry, 0x4AFC);       // illegal
    put16be(rom, kTrapEntry, 0x4E40);          // trap #0
    put16be(rom, kAddrErrEntry + 0, 0x3038);   // move.w ($0001).w,d0
    put16be(rom, kAddrErrEntry + 2, 0x0001);

    put16be(rom, kZ80BusWait + 0x00, 0x33FC);  // move.w #$0000,$A11200
    put16be(rom, kZ80BusWait + 0x02, 0x0000);
    put32be(rom, kZ80BusWait + 0x04, 0x00A11200);
    put16be(rom, kZ80BusWait + 0x08, 0x33FC);  // move.w #$0100,$A11100
    put16be(rom, kZ80BusWait + 0x0A, 0x0100);
    put32be(rom, kZ80BusWait + 0x0C, 0x00A11100);
    put16be(rom, kZ80BusWait + 0x10, 0x0839);  // btst #0,$A11100
    put16be(rom, kZ80BusWait + 0x12, 0x0000);
    put32be(rom, kZ80BusWait + 0x14, 0x00A11100);
    put16be(rom, kZ80BusWait + 0x18, 0x66F6);  // bne.s $570
    put16be(rom, kZ80BusWaitRts, 0x4E75);      // rts

    return rom;
}

// Z80 program, written straight into sound RAM by the test.
//
//   0000  31 F0 1F     ld sp,$1FF0
//   0003  AF           xor a          ; sets Z
//   0004  C4 20 00     call nz,$0020  ; NOT taken — Z is set
//   0007  CD 20 00     call $0020     ; always taken
//   000A  18 FE        jr $000A
//   0020  C9           ret
//
// The untaken conditional call at $0004 is the whole point: counting it as a
// real call is what made the callstack fill with 256 copies of one address.
constexpr uint16_t kZ80Xor        = 0x0003;
constexpr uint16_t kZ80CallNotTaken = 0x0004;
constexpr uint16_t kZ80CallTaken  = 0x0007;
constexpr uint16_t kZ80Loop       = 0x000A;
constexpr uint16_t kZ80Sub        = 0x0020;

// Data-access routine, reached by setting PC — the same job as kDataEntry on
// the 68000 side. Writes then reads its own work RAM.
constexpr uint16_t kZ80DataEntry = 0x0030;
constexpr uint16_t kZ80DataRead  = 0x0035;
constexpr uint16_t kZ80DataAddr  = 0x1000;
constexpr uint8_t  kZ80DataValue = 0x55;

inline std::vector<uint8_t> buildZ80()
{
    std::vector<uint8_t> z(0x40, 0x00);
    const uint8_t prog[] = {
        0x31, 0xF0, 0x1F,       // 0000 ld sp,$1FF0
        0xAF,                   // 0003 xor a
        0xC4, 0x20, 0x00,       // 0004 call nz,$0020
        0xCD, 0x20, 0x00,       // 0007 call $0020
        0x18, 0xFE,             // 000A jr $000A
    };
    for (size_t i = 0; i < sizeof prog; ++i) z[i] = prog[i];
    z[kZ80Sub] = 0xC9;          // 0020 ret

    const uint8_t data[] = {
        0x3E, kZ80DataValue,    // 0030 ld a,$55
        0x32, 0x00, 0x10,       // 0032 ld ($1000),a
        0x3A, 0x00, 0x10,       // 0035 ld a,($1000)
        0x18, 0xFE,             // 0038 jr $0038
    };
    for (size_t i = 0; i < sizeof data; ++i) z[kZ80DataEntry + i] = data[i];
    return z;
}

// Writes the ROM to a temp file and removes it on scope exit. The emulator
// takes a path, so a real file is unavoidable; leaking them is not.
class RomFile {
public:
    explicit RomFile(const std::vector<uint8_t>& rom = buildRom())
    {
#ifdef _WIN32
        const char* tmp = std::getenv("TEMP");
        path_ = std::string(tmp ? tmp : ".") + "\\smd_dgx_test_rom.bin";
#else
        const char* tmp = std::getenv("TMPDIR");
        path_ = std::string(tmp ? tmp : ".") + "/smd_dgx_test_rom.bin";
#endif
        if (FILE* f = std::fopen(path_.c_str(), "wb")) {
            std::fwrite(rom.data(), 1, rom.size(), f);
            std::fclose(f);
            ok_ = true;
        }
    }
    ~RomFile() { if (ok_) std::remove(path_.c_str()); }

    const char* path() const { return path_.c_str(); }
    bool ok() const { return ok_; }

private:
    std::string path_;
    bool ok_ = false;
};

} // namespace synth
