#include "GpgxBackend.h"

extern "C" {
#include <shared.h>           // uint8/uint16/uint32/int16 macros — must be first
#include <m68k.h>             // m68ki_cpu_core m68k, m68k_get_reg, cpu_memory_map
#include <z80.h>              // Z80_Regs Z80
#include <vdp_ctrl.h>         // reg[], vram[], cram[], vsram[], sat[], status
#include <mem68k.h>
#include <genesis.h>          // work_ram[], zram[], cart macro
#include <input_hw/input.h>   // t_input input, MAX_DEVICES
#include <system.h>           // vdp_pal, MCYCLES_PER_LINE
#include <sound/sound.h>      // fm_debug_regs
#include <sound/psg.h>        // psg_debug_regs
#include <state.h>            // state_save/state_load, STATE_SIZE
#include <loadrom.h>          // rominfo: name, serial, calculated checksum
#include <debug/cpuhook.h>
}

#include <fstream>

#include <gx/gx.hpp>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iterator>

GpgxBackend* g_gpgxBackend = nullptr;

// m68k.stopped bits and run mode (core/m68k/m68kcpu.h STOP_LEVEL_*,
// RUN_MODE_NORMAL, INSTRUCTION_YES; that header is private to the core).
// Pinned by the halted/stopped tests in test_cpu_state.cpp.
namespace {
constexpr unsigned kStopLevelStop = 1, kStopLevelHalt = 2, kStopLevelWait = 4;
constexpr unsigned kRunModeNormal = 0, kInstructionYes = 0;
}

static void cpuHookShim(hook_type_t type, int width, unsigned int addr, unsigned int value)
{
    if (g_gpgxBackend)
        g_gpgxBackend->onCpuHook(static_cast<int>(type), width, addr, value);
}

static unsigned int readFilterShim(int width, unsigned int addr, unsigned int value)
{
    return g_gpgxBackend ? g_gpgxBackend->filterRead(width, addr, value) : value;
}

// The read filter stays installed for the backend's lifetime: changing the
// core's function pointer while the emulation thread reads through it would
// be a race. With no overrides it returns the value untouched (one relaxed
// load), and it only ever sees reads that go through an I/O handler.
GpgxBackend::GpgxBackend()
    : exec68kMap_(new std::atomic<uint8_t>[size_t(1) << 21]),
      execZ80Map_(new std::atomic<uint8_t>[size_t(1) << 13])
{
    for (size_t i = 0; i < (size_t(1) << 21); ++i) exec68kMap_[i].store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < (size_t(1) << 13); ++i) execZ80Map_[i].store(0, std::memory_order_relaxed);
    g_gpgxBackend = this;
    set_cpu_hook(cpuHookShim);
    set_cpu_read_filter(readFilterShim);
}

GpgxBackend::~GpgxBackend()
{
    // Only tear down the hook if it is still ours. A second backend claims the
    // global on construction, and an unconditional clear here would silently
    // kill breakpoints and stepping for whoever is actually running.
    if (g_gpgxBackend == this) {
        set_cpu_read_filter(nullptr);
        set_cpu_hook(nullptr);
        g_gpgxBackend = nullptr;
    }
}

SessionInfo GpgxBackend::sessionInfo() const
{
    SessionInfo s;
    if (!running_.load()) return s;
    s.crc = rominfo.realchecksum;
    // The header fields are fixed-width and space-padded, not terminated.
    auto trimmed = [](const char* p, size_t n) {
        std::string v(p, ::strnlen(p, n));
        while (!v.empty() && (unsigned char)v.back() <= 0x20) v.pop_back();
        return v;
    };
    s.serial = trimmed(rominfo.product, sizeof rominfo.product);
    s.name   = trimmed(rominfo.domestic, sizeof rominfo.domestic);
    return s;
}

// ---------------------------------------------------------------------------
// CPU state
// ---------------------------------------------------------------------------
M68kRegs GpgxBackend::getM68kRegs()
{
    M68kRegs r{};
    for (int i = 0; i < 8; ++i) r.d[i] = m68k_get_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i));
    for (int i = 0; i < 8; ++i) r.a[i] = m68k_get_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i));
    r.pc  = m68k_get_reg(M68K_REG_PC);
    r.sr  = m68k_get_reg(M68K_REG_SR);
    r.usp = m68k_get_reg(M68K_REG_USP);
    r.isp = m68k_get_reg(M68K_REG_ISP);
    return r;
}

void GpgxBackend::setM68kRegs(const M68kRegs& r)
{
    // Snapshots expose active A7 and its USP/ISP alias. If A7 was edited,
    // update the selected bank too, instead of restoring its stale snapshot.
    // An SR-only edit must still select the saved bank, and explicit bank-only
    // edits must keep working when A7 itself is unchanged.
    uint32_t usp = r.usp;
    uint32_t isp = r.isp;
    if (r.a[7] != m68k_get_reg(M68K_REG_A7)) {
        if (r.sr & 0x2000) isp = r.a[7];
        else usp = r.a[7];
    }
    // Moving PC restarts the CPU there. A halt (double fault) or a STOP would
    // otherwise outlive it and the CPU would never run again; likewise an
    // unfinished bus/address-error frame would turn the next fault into a halt.
    if ((r.pc & 0xFFFFFF) != (m68k_get_reg(M68K_REG_PC) & 0xFFFFFF)) {
        m68k.stopped &= ~(kStopLevelStop | kStopLevelHalt);
        m68k.run_mode = kRunModeNormal;
        m68k.instr_mode = kInstructionYes;
    }
    for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i), r.d[i]);
    for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i), r.a[i]);
    m68k_set_reg(M68K_REG_PC,  r.pc);
    m68k_set_reg(M68K_REG_SR,  r.sr);
    m68k_set_reg(M68K_REG_USP, usp);
    m68k_set_reg(M68K_REG_ISP, isp);
}

Z80Regs GpgxBackend::getZ80Regs()
{
    Z80Regs r{};
    r.af  = Z80.af.w.l;  r.bc  = Z80.bc.w.l;  r.de  = Z80.de.w.l;  r.hl  = Z80.hl.w.l;
    r.af2 = Z80.af2.w.l; r.bc2 = Z80.bc2.w.l; r.de2 = Z80.de2.w.l; r.hl2 = Z80.hl2.w.l;
    r.ix  = Z80.ix.w.l;  r.iy  = Z80.iy.w.l;
    r.sp  = Z80.sp.w.l;  r.pc  = Z80.pc.w.l;
    r.i   = Z80.i; r.r = Z80.r; r.im = Z80.im;
    r.iff1 = Z80.iff1; r.iff2 = Z80.iff2; r.halt = Z80.halt;
    r.bank = ::zbank;
    return r;
}

void GpgxBackend::setZ80Regs(const Z80Regs& r)
{
    Z80.af.w.l  = r.af;  Z80.bc.w.l  = r.bc;  Z80.de.w.l  = r.de;  Z80.hl.w.l  = r.hl;
    Z80.af2.w.l = r.af2; Z80.bc2.w.l = r.bc2; Z80.de2.w.l = r.de2; Z80.hl2.w.l = r.hl2;
    Z80.ix.w.l  = r.ix;  Z80.iy.w.l  = r.iy;  Z80.sp.w.l  = r.sp;  Z80.pc.w.l  = r.pc;
    Z80.i = r.i; Z80.r = r.r; Z80.im = r.im;
    Z80.iff1 = r.iff1; Z80.iff2 = r.iff2; Z80.halt = r.halt;
}

VdpState GpgxBackend::getVdpState()
{
    VdpState v{};
    std::memcpy(v.reg, reg, sizeof(v.reg));
    v.status   = ::status;
    // reg 19 is the LOW half of the length and reg 20 the high one — the same
    // order the core itself uses (vdp_ctrl.c: (reg[20] << 8) | reg[19]).
    // Swapping them turns a 0x140-word transfer into a nonsensical 0x4001.
    v.dma_len  = ((reg[20] & 0xFF) << 8) | (reg[19] & 0xFF);
    // Registers 21-23 hold a *word* address; report the byte address, which is
    // what every caller actually wants to compare against a memory map.
    v.dma_src  = (uint32_t((reg[21] & 0xFF) | ((reg[22] & 0xFF) << 8)
                           | ((reg[23] & 0x7F) << 16)) << 1) & 0xFFFFFF;
    v.dma_type = ::dma_type;
    v.vram  = ::vram;
    v.cram  = ::cram;
    v.vsram = ::vsram;
    v.sat   = ::sat;
    unsigned int a = 0, c = 0;
    vdp_debug_get_access(&a, &c);
    v.vdp_addr = uint16_t(a);
    v.vdp_code = uint8_t(c);
    return v;
}

void GpgxBackend::setVdpReg(int idx, uint8_t value)
{
    if (idx >= 0 && idx < 0x20) reg[idx] = value;
}

SoundState GpgxBackend::getSoundState()
{
    SoundState s{};
    std::memcpy(s.fm, fm_debug_regs, sizeof(s.fm));
    const int* p = psg_debug_regs();
    for (int i = 0; i < 8; ++i) s.psg[i] = p[i];
    return s;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
// map->base regions (ROM/RAM) are stored word-swapped on LSB_FIRST hosts;
// XOR 1 converts to logical Genesis byte order. read8/write8 handlers take
// real bus addresses and need no correction.
std::vector<uint8_t> GpgxBackend::readMemory(uint32_t addr, uint32_t size)
{
    std::vector<uint8_t> buf(size, 0xFF);
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t a = (addr + i) & 0xFFFFFF;
        const cpu_memory_map* map = &m68k.memory_map[(a >> 16) & 0xFF];
        if (map->base)  buf[i] = map->base[(a & 0xFFFF) ^ 1];
        else if (map->read8) buf[i] = static_cast<uint8_t>(map->read8(a));
    }
    return buf;
}

bool GpgxBackend::writeMemory(uint32_t addr, const uint8_t* data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t a = (addr + i) & 0xFFFFFF;
        cpu_memory_map* map = &m68k.memory_map[(a >> 16) & 0xFF];
        if (map->base) map->base[(a & 0xFFFF) ^ 1] = data[i];
        else if (map->write8) map->write8(a, data[i]);
        else return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Save states — see the threading note on IDebugBackend
// ---------------------------------------------------------------------------
bool GpgxBackend::saveState(const char* path)
{
    if (!path || !*path) return false;
    std::vector<uint8_t> buf(STATE_SIZE);
    const int len = state_save(buf.data());
    if (len <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(buf.data()), len);
    return static_cast<bool>(f);
}

bool GpgxBackend::loadState(const char* path)
{
    if (!path || !*path) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    if (size <= 0 || size > static_cast<std::streamsize>(STATE_SIZE)) return false;
    f.seekg(0);
    std::vector<uint8_t> buf(STATE_SIZE, 0);
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) return false;
    return state_load(buf.data()) != 0;
}

bool GpgxBackend::runSafely(const std::function<void()>& fn)
{
    if (!fn) return false;

    // Preferred: hand it to the host's emulation thread.
    if (safeExec_) return safeExec_(fn);

    // Already paused — the emulation thread is parked in firePause(), so
    // nothing is touching the core.
    if (paused_.load()) { fn(); return true; }

    if (!running_.load()) { fn(); return true; }   // nothing running to race

    // No host queue: stop the world ourselves.
    pause();
    for (int i = 0; i < 500 && !paused_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!paused_.load()) return false;
    fn();
    resume();
    return true;
}

// ---------------------------------------------------------------------------
// Controller input
// ---------------------------------------------------------------------------
void GpgxBackend::setPad(int port, uint16_t buttons)
{
    if (port >= 0 && port < MAX_DEVICES)
        input.pad[port] = buttons;
}

uint16_t GpgxBackend::getPad(int port)
{
    return (port >= 0 && port < MAX_DEVICES) ? input.pad[port] : 0;
}

// ---------------------------------------------------------------------------
// Memory regions (hex editor / RAM tools) — parity with the Gens hex editor
// ---------------------------------------------------------------------------
namespace {
struct RegionDesc {
    int id; const char* name; uint32_t base; bool writable; uint8_t swap;
};
// ids are a stable contract with the views
constexpr RegionDesc kRegions[] = {
    { 0, "ROM",       0x000000, true,  1 },
    { 1, "RAM 68K",   0xFF0000, true,  1 },
    { 2, "RAM Z80",   0xA00000, true,  0 },
    { 3, "VRAM",      0x000000, true,  1 },
    // CRAM/VSRAM hold core-internal packed words, not Genesis bus bytes, and
    // the core reads them as native uint16 — expose them raw (see DebugState.h).
    { 4, "CRAM",      0x000000, true,  0 },
    { 5, "VSRAM",     0x000000, true,  0 },
    { 6, "Regs M68K", 0x000000, true,  0 },
    { 7, "Regs Z80",  0x000000, false, 0 },
    { 8, "Regs VDP",  0x000000, true,  0 },
};

uint8_t* regionArray(int id, uint32_t& size)
{
    switch (id) {
    case 0: size = cart.romsize;  return cart.rom;
    case 1: size = 0x10000;       return work_ram;
    case 2: size = 0x2000;        return zram;
    case 3: size = 0x10000;       return vram;
    case 4: size = 0x80;          return cram;
    case 5: size = 0x80;          return vsram;
    case 8: size = 0x20;          return reg;
    default: size = 0;            return nullptr;
    }
}

void putBE32(std::vector<uint8_t>& v, uint32_t off, uint32_t x)
{
    if (off + 3 < v.size()) {
        v[off] = x >> 24; v[off+1] = x >> 16; v[off+2] = x >> 8; v[off+3] = x;
    }
}
void putBE16(std::vector<uint8_t>& v, uint32_t off, uint16_t x)
{
    if (off + 1 < v.size()) { v[off] = x >> 8; v[off+1] = (uint8_t)x; }
}
} // namespace

std::vector<MemRegion> GpgxBackend::getMemRegions()
{
    std::vector<MemRegion> out;
    for (const auto& r : kRegions) {
        MemRegion m;
        m.id = r.id; m.name = r.name; m.base = r.base; m.writable = r.writable;
        switch (r.id) {
        case 6: m.size = 64; break;               // D0-D7,A0-A7 as BE dwords
        case 7: m.size = 24; break;               // 12 BE word pairs
        default: { uint32_t s = 0; regionArray(r.id, s); m.size = s; }
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<uint8_t> GpgxBackend::readRegion(int id, uint32_t off, uint32_t size)
{
    if (id == 6) { // M68K regs: D0..D7, A0..A7 as big-endian dwords
        std::vector<uint8_t> full(64, 0);
        M68kRegs r = getM68kRegs();
        for (int i = 0; i < 8; ++i) { putBE32(full, i*4, r.d[i]); putBE32(full, 32 + i*4, r.a[i]); }
        std::vector<uint8_t> out;
        for (uint32_t i = 0; i < size && off + i < full.size(); ++i) out.push_back(full[off + i]);
        return out;
    }
    if (id == 7) { // Z80 regs: AF,BC,DE,HL,AF',BC',DE',HL',IX,IY,SP,PC as BE words
        std::vector<uint8_t> full(24, 0);
        Z80Regs z = getZ80Regs();
        const uint16_t regs16[12] = { z.af, z.bc, z.de, z.hl, z.af2, z.bc2, z.de2, z.hl2, z.ix, z.iy, z.sp, z.pc };
        for (int i = 0; i < 12; ++i) putBE16(full, i*2, regs16[i]);
        std::vector<uint8_t> out;
        for (uint32_t i = 0; i < size && off + i < full.size(); ++i) out.push_back(full[off + i]);
        return out;
    }

    uint32_t rsize = 0;
    uint8_t* arr = regionArray(id, rsize);
    const uint8_t swap = (id >= 0 && id < (int)std::size(kRegions)) ? kRegions[id].swap : 0;
    std::vector<uint8_t> out;
    if (!arr) return out;
    out.reserve(size);
    for (uint32_t i = 0; i < size && off + i < rsize; ++i)
        out.push_back(arr[(off + i) ^ swap]);
    return out;
}

bool GpgxBackend::writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size)
{
    if (id == 7) return false;
    if (id == 6) { // poke M68K regs through the byte image
        std::vector<uint8_t> full = readRegion(6, 0, 64);
        if (full.size() < 64) return false;
        for (uint32_t i = 0; i < size && off + i < 64; ++i) full[off + i] = data[i];
        M68kRegs r = getM68kRegs();
        for (int i = 0; i < 8; ++i) {
            r.d[i] = (full[i*4] << 24) | (full[i*4+1] << 16) | (full[i*4+2] << 8) | full[i*4+3];
            r.a[i] = (full[32+i*4] << 24) | (full[32+i*4+1] << 16) | (full[32+i*4+2] << 8) | full[32+i*4+3];
        }
        setM68kRegs(r);
        return true;
    }

    uint32_t rsize = 0;
    uint8_t* arr = regionArray(id, rsize);
    if (!arr) return false;
    const uint8_t swap = (id >= 0 && id < (int)std::size(kRegions)) ? kRegions[id].swap : 0;
    if (!kRegions[id].writable) return false;
    for (uint32_t i = 0; i < size && off + i < rsize; ++i)
        arr[(off + i) ^ swap] = data[i];
    return true;
}

// The Z80's own 16-bit view. Only the parts that can be read without side
// effects are served: $4000 (YM2612) and $7F00 (VDP) are live hardware ports
// where a read changes state, so they stay 0xFF rather than being "helpfully"
// sampled behind the driver's back.
std::vector<uint8_t> GpgxBackend::readZ80Memory(uint16_t addr, uint16_t size)
{
    std::vector<uint8_t> buf(size, 0xFF);
    for (uint16_t i = 0; i < size; ++i) {
        const uint16_t a = (uint16_t)(addr + i);
        if (a < 0x4000) {
            buf[i] = ::zram[a & 0x1FFF];            // 8K, mirrored through $3FFF
        } else if (a >= 0x8000) {
            // Bank window into 68000 space. Same word-swap correction as
            // readMemory: the core stores ROM/RAM byte-swapped on LSB_FIRST.
            const uint32_t b = (::zbank | (a & 0x7FFF)) & 0xFFFFFF;
            const cpu_memory_map* map = &m68k.memory_map[(b >> 16) & 0xFF];
            if (map->base) buf[i] = map->base[(b & 0xFFFF) ^ 1];
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Breakpoints
// ---------------------------------------------------------------------------
int GpgxBackend::addBreakpoint(const Breakpoint& bp)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    Breakpoint b = bp; b.id = nextBpId_++;
    breakpoints_.push_back(b);
    markExec(b, true);
    indexBreakpoints(nullptr);
    return b.id;
}
void GpgxBackend::removeBreakpoint(int id)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (it->id != id) continue;
        const Breakpoint gone = *it;
        breakpoints_.erase(it);
        indexBreakpoints(&gone);
        return;
    }
}
bool GpgxBackend::updateBreakpoint(const Breakpoint& bp)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    for (auto& b : breakpoints_)
        if (b.id == bp.id) {
            const Breakpoint old = b;
            b = bp;
            indexBreakpoints(&old);     // unmark the old range, re-mark what remains
            markExec(b, true);
            return true;
        }
    return false;
}
void GpgxBackend::clearBreakpoints()
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    for (const Breakpoint& b : breakpoints_) markExec(b, false);
    breakpoints_.clear();
    indexBreakpoints(nullptr);
}

// Set or clear the address bits an enabled execute breakpoint covers.
void GpgxBackend::markExec(const Breakpoint& b, bool on)
{
    if (b.type != BpType::PC || b.is_vdp || (on && !b.enabled)) return;
    const bool z80 = b.cpu == Cpu::Z80;
    auto& map = z80 ? execZ80Map_ : exec68kMap_;
    const uint32_t mask = z80 ? 0xFFFFu : 0xFFFFFFu;
    if (b.end < b.start) return;
    const uint32_t first = b.start & mask, last = std::min(b.end, mask);
    for (uint32_t a = first; ; ++a) {
        const uint8_t bit = uint8_t(1u << (a & 7));
        if (on) map[a >> 3].fetch_or(bit, std::memory_order_relaxed);
        else    map[a >> 3].fetch_and(uint8_t(~bit), std::memory_order_relaxed);
        if (a == last) break;
    }
}

// Recount by kind; after a removal (or the old version of an updated one),
// clear its bits and re-mark the remaining breakpoints that overlap them.
void GpgxBackend::indexBreakpoints(const Breakpoint* removed)
{
    if (removed && removed->type == BpType::PC && !removed->is_vdp) {
        markExec(*removed, false);
        for (const Breakpoint& b : breakpoints_)
            if (b.cpu == removed->cpu && b.start <= removed->end && b.end >= removed->start)
                markExec(b, true);
    }
    int e68 = 0, ez = 0, rw = 0;
    for (const Breakpoint& b : breakpoints_) {
        if (!b.enabled) continue;
        if (b.type == BpType::PC && !b.is_vdp) (b.cpu == Cpu::Z80 ? ez : e68)++;
        else ++rw;
    }
    exec68kCount_.store(e68);
    execZ80Count_.store(ez);
    rwCount_.store(rw);
}

bool GpgxBackend::execMayHit(Cpu cpu, uint32_t addr, int width) const
{
    const bool z80 = cpu == Cpu::Z80;
    if ((z80 ? execZ80Count_ : exec68kCount_).load(std::memory_order_relaxed) == 0) return false;
    const auto& map = z80 ? execZ80Map_ : exec68kMap_;
    const uint32_t mask = z80 ? 0xFFFFu : 0xFFFFFFu;
    for (int i = 0; i < (width > 0 ? width : 1); ++i) {
        const uint32_t a = (addr + uint32_t(i)) & mask;
        if (map[a >> 3].load(std::memory_order_relaxed) & (1u << (a & 7))) return true;
    }
    return false;
}
std::vector<Breakpoint> GpgxBackend::getBreakpoints() { std::lock_guard<std::mutex> lk(bpMutex_); return breakpoints_; }

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
// A pause asked of a machine already paused is satisfied: arming it would stop
// whatever runs next, one instruction in, for no reason anyone remembers.
void GpgxBackend::pause()   { requestPause(StopReason::Pause); }
void GpgxBackend::requestPause(StopReason why)
{
    std::lock_guard<std::mutex> lk(reqMx_);
    if (paused_.load()) return;
    pauseReason_.store(uint8_t(why));
}

void GpgxBackend::serviceStoppedCpu()
{
    // Only a request the instruction hook could not deliver, or an armed run:
    // the CPU retired no instruction during a whole frame. A STOP that a VBlank
    // will end is left alone (it retires the handler's instructions every
    // frame), so stepping through it still lands in the interrupt handler.
    const bool idleFrame = execCount_ == execCountAtFrame_;
    execCountAtFrame_ = execCount_;
    const unsigned stopped = m68k.stopped;
    if (!idleFrame || !(stopped & (kStopLevelStop | kStopLevelHalt | kStopLevelWait))) return;
    const bool step = stepInto_.load();
    // An armed run (runto, runcase) can never reach its return, address or
    // instruction budget on a CPU that executes nothing: end it now rather
    // than let it run out a wall-clock timeout.
    const bool run = runActive_.load();
    if (!pauseReason_.load() && !step && !run) return;
    runActive_.store(false);
    pauseReason_.store(0);
    stepInto_.store(false);
    // This stop is not inside an instruction hook, so the next hook is the
    // instruction we are standing on, not the one after it: let it run.
    standingOnNext_ = true;
    stopInInstr_ = false;
    stopMidInstr_ = false;
    firePause(m68k_get_reg(M68K_REG_PC), Cpu::M68K, -1,
              (stopped & kStopLevelHalt) ? StopReason::Halted : StopReason::Stopped);
}
// Every transition to running happens under reqMx_, like firePause's stop
// publication: a resume can then never fall between a stop taking its run
// epoch and the machine being marked paused (which would leave it paused at a
// stop a host discards as already run past).
void GpgxBackend::startRunning()
{
    std::lock_guard<std::mutex> lk(reqMx_);
    beginRun();
    paused_.store(false);
}

void GpgxBackend::resume()
{
    startRunning();
    announceResumed();
}
void GpgxBackend::cancelRunRequests()
{
    pauseReason_.store(0);
    stepInto_.store(false);
    stepIntoZ80_.store(false);
    stepOverAddr_.store(-1);
    stepOverAddrZ80_.store(-1);
    framePauseReq_.store(false);
}

// Arming takes the stop lock, so a stop being published (which clears every
// pending request) cannot clear a request armed for the run that follows it.
void GpgxBackend::armStepInto(Cpu cpu) { std::lock_guard<std::mutex> lk(reqMx_); armStepIntoLocked(cpu); }
void GpgxBackend::armStepOver(Cpu cpu) { std::lock_guard<std::mutex> lk(reqMx_); armStepOverLocked(cpu); }
void GpgxBackend::requestFramePause()  { std::lock_guard<std::mutex> lk(reqMx_); framePauseReq_.store(true); }

void GpgxBackend::armStepIntoLocked(Cpu cpu)
{
    if (cpu == Cpu::Z80) stepIntoZ80_.store(true);
    else                 stepInto_.store(true);
}
bool GpgxBackend::resumeIfEpoch(uint64_t epoch)
{
    {
        std::lock_guard<std::mutex> lk(reqMx_);
        if (runEpoch_.load() != epoch) return false;
        paused_.store(false);
    }
    if (resumeCb_) resumeCb_();
    return true;
}

bool GpgxBackend::stepIfEpoch(Cpu cpu, bool over, uint64_t epoch)
{
    {
        std::lock_guard<std::mutex> lk(reqMx_);
        if (runEpoch_.load() != epoch) return false;
        if (over) armStepOverLocked(cpu); else armStepIntoLocked(cpu);
        paused_.store(false);
    }
    if (resumeCb_) resumeCb_();
    return true;
}

void GpgxBackend::stepInto(Cpu cpu)
{
    std::lock_guard<std::mutex> lk(reqMx_);
    armStepIntoLocked(cpu);
    beginRun();
    paused_.store(false);
}

void GpgxBackend::stepOver(Cpu cpu)
{
    armStepOver(cpu);
    resume();
}

void GpgxBackend::armStepOverLocked(Cpu cpu)
{
    if (cpu == Cpu::Z80) { armStepOverZ80(); return; }

    uint32_t pc = m68k_get_reg(M68K_REG_PC);
    const cpu_memory_map* map = &m68k.memory_map[(pc >> 16) & 0xFF];
    uint16_t opc = 0;
    // storage is word-swapped on LE hosts: logical hi byte lives at addr^1
    if (map->base) opc = (static_cast<uint16_t>(map->base[(pc & 0xFFFF) ^ 1]) << 8) | map->base[((pc+1) & 0xFFFF) ^ 1];
    if ((opc & 0xFFC0) == 0x4E80) {
        // jsr <ea>: how far to step depends on the addressing mode, since the
        // extension words are part of the instruction. Getting this wrong sets
        // the resume address inside the operand and the step never lands.
        const int mod = (opc >> 3) & 7;
        const int reg = opc & 7;
        int words = 1;                       // the opcode itself

        auto countExtension = [&] {
            // Brief format is one word. Bit 8 selects the 68020 full format,
            // which a 68000 never produces — handled anyway so a 68020 database
            // does not mis-step.
            const uint16_t ext = opcodeAt(pc + 2);
            words += 1;
            if (ext & 0x0100) {
                const int bd = (ext >> 4) & 3;   // base displacement
                const int od = ext & 3;          // outer displacement
                if (bd == 2) words += 1; else if (bd == 3) words += 2;
                if (od == 2) words += 1; else if (od == 3) words += 2;
            }
        };

        switch (mod) {
        case 2: break;                       // (An)
        case 5: words += 1; break;           // (d16,An)
        case 6: countExtension(); break;     // (d8,An,Xn)
        case 7:
            switch (reg) {
            case 0: words += 1; break;       // (xxx).W
            case 1: words += 2; break;       // (xxx).L
            case 2: words += 1; break;       // (d16,PC)
            case 3: countExtension(); break; // (d8,PC,Xn)
            default: break;
            }
            break;
        default: break;                      // not a valid jsr destination
        }
        stepOverAddr_.store(static_cast<int>(pc + words * 2));
    } else if ((opc & 0xFF00) == 0x6100) {
        int off = (opc & 0xFF) == 0 ? 2 : (opc & 0xFF) == 0xFF ? 3 : 1;
        stepOverAddr_.store(static_cast<int>(pc + off * 2));
    } else {
        // Nothing to step over: a plain step. (It used to arm the step without
        // running, leaving it pending for whatever resumed the machine next.)
        stepInto_.store(true);
    }
}

bool GpgxBackend::loadRom(const char* path)
{
    gx::init();

    // gx::init() zeroes the bitmap descriptor but allocates no framebuffer —
    // supplying it is the host's job, and the VDP renderer writes through the
    // pointer on the very first frame. Own it here so every host (standalone
    // frontend, IDA plugin, anything future) is safe by construction.
    // Sized for the largest geometry gpgx can produce at 32bpp.
    static std::vector<uint8_t> framebuffer(720 * 576 * 4, 0);
    gx::bitmap_data() = framebuffer.data();

    { std::lock_guard<std::mutex> lk(callstackMutex_); callstack_.clear(); callstackZ80_.clear(); }
    abandoned_.store(false);        // a previous shutdown must not mute this run
    standingOnNext_ = false;
    execCount_ = execCountAtFrame_ = 0;
    runActive_.store(false);

    bool ok = gx::load_rom(path);
    if (ok) running_.store(true);
    return ok;
}

// ---------------------------------------------------------------------------
// CPU hook
// ---------------------------------------------------------------------------
void GpgxBackend::onCpuHook(int type, int width, uint32_t addr, uint32_t value)
{
    if (type & HOOK_M68K_E) {
        // codemap: record predecessor of every executed ROM/RAM address
        if (lastPc_ && addr && addr < MAXROMSIZE) {
            std::lock_guard<std::mutex> lk(codemapMutex_);
            codemap_[addr] = lastPc_;
        }
        lastPc_ = addr;
        ++execCount_;
        trackCall(addr);
        // Resuming never stops on the instruction the CPU stands on (as with a
        // stop taken inside this hook, where that instruction just resumes and
        // a PC moved meanwhile runs without a hook at all): continue steps over
        // a breakpoint at the current PC. See test_cpu_state.cpp.
        if (standingOnNext_) { standingOnNext_ = false; return; }
        StopReason why = StopReason(pauseReason_.exchange(0));
        if (stepInto_.exchange(false) && why == StopReason::None) why = StopReason::Step;
        if (why == StopReason::None) {
            int so = stepOverAddr_.load();
            if (so >= 0 && (uint32_t)so == addr) { stepOverAddr_.store(-1); why = StopReason::Step; }
        }
        if (why == StopReason::None && runActive_.load(std::memory_order_relaxed)) {
            const RunLimits& lim = run_;
            if (lim.toReturn && addr == lim.returnAddr && m68k_get_reg(M68K_REG_A7) > lim.entrySp)
                why = StopReason::RunToReturn;
            else if (lim.stopAddr >= 0 && addr == uint32_t(lim.stopAddr))
                why = StopReason::StopAddress;
            else if (lim.maxInstructions && execCount_ - runStartExec_ >= lim.maxInstructions)
                why = StopReason::Budget;
            if (why != StopReason::None) runActive_.store(false);
        }
        int hit = -1;
        if (why == StopReason::None) {
            hit = matchBreakpoint(type, addr, 2);                       // opcode word
            if (hit >= 0) why = StopReason::Breakpoint;
        }
        if (why != StopReason::None) {
            stopInInstr_ = true;                // at its start: counted, retires next
            stopMidInstr_ = false;
            firePause(addr, Cpu::M68K, hit, why);
        }
    } else if (type & (HOOK_M68K_R | HOOK_M68K_W)) {
        if (logOn_.load(std::memory_order_relaxed))
            logAccess(m68k.ppc & 0xFFFFFF, Cpu::M68K, AccessSpace::M68k, (type & HOOK_M68K_W) != 0, width, addr, value);
        const int hit = matchBreakpoint(type, addr, width);
        if (hit >= 0) watchHit(hit, m68k.ppc & 0xFFFFFF, Cpu::M68K, AccessSpace::M68k, (type & HOOK_M68K_W) != 0,
                               width, addr, value);
    } else if (type & HOOK_M68K_EXC) {
        onM68kException(value);
    } else if (type & HOOK_Z80_E) {
        onZ80Exec(addr & 0xFFFF);
    } else if (type & (HOOK_Z80_R | HOOK_Z80_W)) {
        // Report the Z80's own PC, not the 68000's: a sound driver writing to
        // its work RAM is a Z80 event, and stopping at a 68000 address would
        // point the user at code that has nothing to do with it.
        if (logOn_.load(std::memory_order_relaxed))
            logAccess(lastPcZ80_, Cpu::Z80, AccessSpace::Z80, (type & HOOK_Z80_W) != 0, width, addr & 0xFFFF, value);
        const int hit = matchBreakpoint(type, addr & 0xFFFF, width);
        if (hit >= 0) watchHit(hit, lastPcZ80_, Cpu::Z80, AccessSpace::Z80, (type & HOOK_Z80_W) != 0,
                               width, addr & 0xFFFF, value);
    } else if (type & (HOOK_VRAM_R | HOOK_VRAM_W | HOOK_CRAM_R | HOOK_CRAM_W |
                       HOOK_VSRAM_R | HOOK_VSRAM_W)) {
        // The core reports an offset within one VDP memory; breakpoints live in
        // the combined space, so bias it the same way the IDA plugin does.
        uint32_t linear = addr & 0xFFFF;
        if (type & (HOOK_CRAM_R  | HOOK_CRAM_W))  linear += VDP_BP_CRAM;
        else if (type & (HOOK_VSRAM_R | HOOK_VSRAM_W)) linear += VDP_BP_VSRAM;
        if (logOn_.load(std::memory_order_relaxed))
            logAccess(m68k.ppc & 0xFFFFFF, Cpu::M68K, AccessSpace::Vdp,
                      (type & (HOOK_VRAM_W | HOOK_CRAM_W | HOOK_VSRAM_W)) != 0, width, linear, value);
        const int hit = matchBreakpoint(type, linear, width);
        if (hit >= 0) watchHit(hit, m68k.ppc & 0xFFFFFF, Cpu::M68K, AccessSpace::Vdp,
                               (type & (HOOK_VRAM_W | HOOK_CRAM_W | HOOK_VSRAM_W)) != 0,
                               width, linear, value);
    }
}

RunLimits RunLimits::toReturnFromHere(GpgxBackend& b)
{
    RunLimits lim;
    const uint32_t sp = b.getM68kRegs().a[7];
    const auto ret = b.readMemory(sp, 4);
    lim.toReturn   = true;
    lim.entrySp    = sp;
    lim.returnAddr = ((uint32_t(ret[0]) << 24) | (uint32_t(ret[1]) << 16) |
                      (uint32_t(ret[2]) << 8) | ret[3]) & 0xFFFFFF;
    return lim;
}

void GpgxBackend::armRun(const RunLimits& lim)
{
    runActive_.store(false);
    run_ = lim;
    run_.returnAddr &= 0xFFFFFF;
    if (run_.stopAddr >= 0) run_.stopAddr &= 0xFFFFFF;
    // Budget = 68000 instructions retired from here. A stop taken inside a
    // 68000 instruction (its exec hook, or one of its accesses) has already
    // counted that instruction, which still retires. Any other stop (between
    // frames, on the Z80, on a caught exception whose instruction never
    // retires) leaves the next counted instruction unretired: start one later.
    runStartExec_ = execCount_ + (stopInInstr_ ? 0 : 1);
    runFrames_ = 0;
    runActive_.store(lim.any());
}

// Is the 68000 inside an instruction right now (called from a non-68000 hook)?
// The core flags the execution of each instruction explicitly: a Z80 run
// that one of its writes synchronised (BUSREQ/RESET) happens inside it.
bool GpgxBackend::m68kMidInstruction() const
{
    return m68k_in_instruction != 0;
}

void GpgxBackend::serviceFramePause()
{
    if (!framePauseReq_.exchange(false)) return;
    // Between frames: the scheduler's per-frame state is fresh from here, so
    // a save state loaded now replays exactly. The next 68000 hook is the
    // instruction the CPU stands on.
    stopInInstr_ = false;
    stopMidInstr_ = false;
    standingOnNext_ = true;
    atFrameBoundary_.store(true);
    firePause(m68k_get_reg(M68K_REG_PC), Cpu::M68K, -1, StopReason::FrameAdvance);
    atFrameBoundary_.store(false);
}

void GpgxBackend::onFrameEnd()
{
    if (!runActive_.load(std::memory_order_relaxed) || !run_.maxFrames) return;
    if (++runFrames_ >= run_.maxFrames) {
        runActive_.store(false);
        requestPause(StopReason::Budget);
    }
}

void GpgxBackend::setReadOverrides(const std::vector<ReadOverride>& v)
{
    std::lock_guard<std::mutex> lk(ovrMx_);
    overrides_ = v;
    ovrActive_.store(!v.empty());
}

std::vector<ReadOverride> GpgxBackend::readOverrides() const
{
    std::lock_guard<std::mutex> lk(ovrMx_);
    return overrides_;
}

uint32_t GpgxBackend::filterRead(int width, uint32_t address, uint32_t value)
{
    if (!ovrActive_.load(std::memory_order_relaxed)) return value;
    std::lock_guard<std::mutex> lk(ovrMx_);
    for (const auto& o : overrides_)
        if (o.size == width && o.address == (address & 0xFFFFFF))
            value = (value & ~o.mask) | (o.value & o.mask);
    return value;
}

void GpgxBackend::startAccessLog(const AccessLogConfig& cfg)
{
    std::lock_guard<std::mutex> lk(logMx_);
    logCfg_ = cfg;
    if (logCfg_.capacity > (size_t(1) << 22)) logCfg_.capacity = size_t(1) << 22;
    log_ = AccessLog{};
    log_.enabled = true;
    log_.entries.reserve(std::min<size_t>(logCfg_.capacity, 4096));
    logOn_.store(true);
}

void GpgxBackend::stopAccessLog()
{
    logOn_.store(false);
    std::lock_guard<std::mutex> lk(logMx_);
    log_.enabled = false;
}

AccessLog GpgxBackend::accessLog() const
{
    std::lock_guard<std::mutex> lk(logMx_);
    return log_;
}

void GpgxBackend::logAccess(uint32_t pc, Cpu cpu, AccessSpace space, bool write,
                            int width, uint32_t addr, uint32_t value)
{
    std::lock_guard<std::mutex> lk(logMx_);
    if (!log_.enabled) return;
    if (!logCfg_.ranges.empty()) {
        const uint32_t last = addr + uint32_t(width > 0 ? width - 1 : 0);
        bool hit = false;
        for (const auto& r : logCfg_.ranges)
            if (r.space == space && last >= r.start && addr <= r.end) { hit = true; break; }
        if (!hit) return;
    }
    if (log_.entries.size() >= logCfg_.capacity) { ++log_.dropped; return; }
    AccessLogEntry e;
    e.pc  = pc & (cpu == Cpu::Z80 ? 0xFFFF : 0xFFFFFF);
    e.cpu = cpu;
    e.access.space   = space;
    e.access.write   = write;
    e.access.size    = uint8_t(width);
    e.access.address = addr;
    e.access.value   = width >= 4 ? value : value & ((1u << (8 * width)) - 1);
    log_.entries.push_back(e);
}

AccessInfo GpgxBackend::lastAccess() const
{
    std::lock_guard<std::mutex> lk(excMx_);
    return lastAccess_;
}

void GpgxBackend::watchHit(int bpId, uint32_t pc, Cpu cpu, AccessSpace space, bool write,
                           int width, uint32_t addr, uint32_t value)
{
    {
        std::lock_guard<std::mutex> lk(excMx_);
        lastAccess_.space   = space;
        lastAccess_.write   = write;
        lastAccess_.size    = uint8_t(width);
        lastAccess_.address = addr;
        lastAccess_.value   = width >= 4 ? value : value & ((1u << (8 * width)) - 1);
    }
    // Inside a 68000 instruction or not is what the core says, whoever made
    // the access: a 68000 instruction; a Z80 run that one of its writes
    // synchronised (BUSREQ/RESET); or neither, e.g. a DMA transfer the
    // scheduler runs between instructions. A 68000-bus access outside an
    // instruction is exception processing (stacking a frame), which is just
    // as unfinished.
    stopInInstr_ = m68kMidInstruction();
    stopMidInstr_ = stopInInstr_ || space == AccessSpace::M68k;
    firePause(pc, cpu, bpId, StopReason::Watchpoint);
}

ExceptionInfo GpgxBackend::lastException() const
{
    std::lock_guard<std::mutex> lk(excMx_);
    return lastExc_;
}

// Called from the core with the frame already stacked and SR already switched
// to supervisor mode, just before the vector is read. The frame holds what the
// exception knows: the old SR, the stacked PC, and for a bus/address error the
// access that faulted.
void GpgxBackend::onM68kException(unsigned vector)
{
    ExceptionInfo x;
    x.vector = uint8_t(vector);
    x.pc = m68k.ppc & 0xFFFFFF;                  // start of the faulting instruction
    const uint32_t sp = m68k_get_reg(M68K_REG_A7);
    // Direct memory only: a stack pointed at I/O must not trigger read handlers.
    auto frame = [](uint32_t at, uint32_t n) {
        std::vector<uint8_t> b(n, 0);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t a = (at + i) & 0xFFFFFF;
            const cpu_memory_map* map = &m68k.memory_map[(a >> 16) & 0xFF];
            if (map->base) b[i] = map->base[(a & 0xFFFF) ^ 1];
        }
        return b;
    };
    auto be16 = [](const std::vector<uint8_t>& b, size_t o) { return uint16_t((b[o] << 8) | b[o + 1]); };
    auto be32 = [&](const std::vector<uint8_t>& b, size_t o) { return (uint32_t(be16(b, o)) << 16) | be16(b, o + 2); };
    if (vector == 2 || vector == 3) {
        // status(R/W I/N FC), access address, IR, SR, PC
        const auto f = frame(sp, 14);
        const uint16_t status = be16(f, 0);
        x.address = be32(f, 2) & 0xFFFFFF;
        x.sr      = be16(f, 8);
        x.framePc = be32(f, 10) & 0xFFFFFF;
        const bool program = (status & 3) == 2;  // FC 2/6: program space
        x.access = (status & 0x10) ? (program ? ExcAccess::Fetch : ExcAccess::Read) : ExcAccess::Write;
    } else {
        const auto f = frame(sp, 6);                // SR, PC
        x.sr      = be16(f, 0);
        x.framePc = be32(f, 2) & 0xFFFFFF;
    }
    { std::lock_guard<std::mutex> lk(excMx_); lastExc_ = x; }
    if (excCb_) excCb_(x);
    if (vector < 64 && (excCatch_.load() >> vector) & 1) {
        const uint32_t before = m68k.pc;
        stopInInstr_ = false;                   // the faulting instruction never retires
        stopMidInstr_ = true;                   // ...but the exception is unfinished
        firePause(x.pc, Cpu::M68K, -1, StopReason::Exception);
        // Moving PC here abandons the vector (see m68ki_jump_vector), and the
        // CPU then stands on the new PC like after any other stop: the next
        // instruction hook must execute it, not stop on it.
        if (m68k.pc != before) standingOnNext_ = true;
    }
}

std::map<uint32_t, uint32_t> GpgxBackend::takeCodemap()
{
    std::lock_guard<std::mutex> lk(codemapMutex_);
    std::map<uint32_t, uint32_t> out;
    out.swap(codemap_);
    return out;
}

void GpgxBackend::abortRunControl()
{
    abandoned_.store(true);
    runActive_.store(false);
    pauseReason_.store(0);
    stepInto_.store(false);
    stepIntoZ80_.store(false);
    stepOverAddr_.store(-1);
    stepOverAddrZ80_.store(-1);
    resume();
}

void GpgxBackend::firePause(uint32_t pc, Cpu cpu, int bpId, StopReason why)
{
    if (abandoned_.load()) return;      // shutting down: do not park the thread
    lastBpId_.store(bpId);
    lastReason_.store(uint8_t(why));
    {
        // This stop answers every request still pending — a pause, a step, a
        // frame-boundary pause — whichever of them it was meant for. Left
        // armed, one would stop some later, unrelated run.
        std::lock_guard<std::mutex> lk(reqMx_);
        stopEpoch_.store(runEpoch_.load());
        if (stopSectionHook_) stopSectionHook_(0);
        paused_.store(true);
        if (stopSectionHook_) stopSectionHook_(1);
        cancelRunRequests();
        stopsFired_.fetch_add(1);
    }
    if (pauseCb_) pauseCb_(pc, cpu);
    // Block the emulation thread until resumed. While blocked, pump host
    // commands (resume/step/read) so run-control can proceed — nested-loop
    // model of the original Gens debugger.
    while (paused_.load() && !abandoned_.load()) {
        if (pausePump_) pausePump_();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Returns the id of the breakpoint that matched, or -1. Which one matched is
// what an agent actually needs: it sets several and then has to know which of
// them it is now looking at.
int GpgxBackend::matchBreakpoint(int type, uint32_t addr, int width)
{
    // A VDP access carries an address in the VDP linear space, so it may only
    // match breakpoints marked is_vdp — and a bus breakpoint must never be
    // triggered by one. Without this the two address spaces alias and VDP
    // breakpoints fire on unrelated RAM.
    const bool vdpAccess = (type & (HOOK_VRAM_R | HOOK_VRAM_W |
                                    HOOK_CRAM_R | HOOK_CRAM_W |
                                    HOOK_VSRAM_R | HOOK_VSRAM_W)) != 0;
    // Both CPUs live in one address space as far as a bare number goes, so a
    // breakpoint must name its processor or a Z80 address matches 68000 code.
    const Cpu cpu = (type & (HOOK_Z80_E | HOOK_Z80_R | HOOK_Z80_W)) ? Cpu::Z80 : Cpu::M68K;

    const bool isRead  = (type & (HOOK_M68K_R | HOOK_Z80_R
                                | HOOK_VRAM_R | HOOK_CRAM_R | HOOK_VSRAM_R)) != 0;
    const bool isWrite = (type & (HOOK_M68K_W | HOOK_Z80_W
                                | HOOK_VRAM_W | HOOK_CRAM_W | HOOK_VSRAM_W)) != 0;
    const bool isExec  = (type & (HOOK_M68K_E | HOOK_Z80_E)) != 0;

    // Hot path, no lock: nothing can match here.
    if (isExec ? !execMayHit(cpu, addr, width) : rwCount_.load(std::memory_order_relaxed) == 0)
        return -1;

    // Collect matches under the lock, evaluate conditions outside it: the
    // evaluator calls into the client (IDA), which must not be done while
    // holding a lock the client's own thread may end up waiting on.
    std::vector<std::tuple<int, uint32_t, std::string>> pending;   // id + elang + expr
    {
        std::lock_guard<std::mutex> lk(bpMutex_);
        for (const auto& bp : breakpoints_) {
            if (!bp.enabled) continue;
            if (bp.cpu != cpu) continue;
            if (bp.is_vdp != vdpAccess) continue;
            const bool typeOk = (bp.type == BpType::PC    && isExec)
                             || (bp.type == BpType::Read  && isRead)
                             || (bp.type == BpType::Write && isWrite);
            if (!typeOk) continue;
            // Overlap, not containment: a longword write at FF4276 touches
            // FF4279, so a breakpoint on FF4278 has to see it. Comparing only
            // the start address silently misses every unaligned access.
            const uint32_t last = addr + uint32_t(width > 0 ? width - 1 : 0);
            if (last < bp.start || addr > bp.end) continue;

            if (bp.condition.empty()) return bp.id;         // unconditional: done
            pending.emplace_back(bp.id, bp.elang, bp.condition);
        }
    }

    if (pending.empty()) return -1;
    // Nobody can judge the conditions; err on stopping rather than skipping.
    if (!condEval_) return std::get<0>(pending.front());

    for (const auto& c : pending)
        if (condEval_(std::get<1>(c), std::get<2>(c))) return std::get<0>(c);
    return -1;
}


// ---------------------------------------------------------------------------
// Z80
//
// Deliberately parallel to the 68000 path rather than shared: the two CPUs run
// interleaved on one thread, and mixing their step flags, PCs or callstacks
// means whichever executes more instructions (always the Z80) wins every race.
// lastPcZ80_ is likewise separate — feeding Z80 addresses into the 68000
// codemap would have IDA turn sound-RAM offsets into 68000 code.
// ---------------------------------------------------------------------------
uint8_t GpgxBackend::z80ByteAt(uint16_t pc) const
{
    return (pc < 0x2000) ? zram[pc] : 0;   // table read: never touch IO handlers
}

// Is the condition in a CALL cc,nn / RET cc satisfied right now?
//
// The hook fires before the instruction executes, so the flags still hold the
// values the instruction is about to test. Checking them is the only way to
// know whether the call is actually taken — and it matters: a sound driver
// polling a flag with "call nz,handler" executes that opcode thousands of
// times a second and takes it almost never. Counting them all made the stack
// pile up until it hit the cap, which is exactly what a bogus 256-deep
// callstack of one repeated address looks like.
static bool z80CondTrue(uint8_t op, uint16_t af)
{
    const uint8_t f = uint8_t(af & 0xFF);
    switch ((op >> 3) & 7) {
    case 0: return (f & 0x40) == 0;   // NZ
    case 1: return (f & 0x40) != 0;   // Z
    case 2: return (f & 0x01) == 0;   // NC
    case 3: return (f & 0x01) != 0;   // C
    case 4: return (f & 0x04) == 0;   // PO
    case 5: return (f & 0x04) != 0;   // PE
    case 6: return (f & 0x80) == 0;   // P
    default: return (f & 0x80) != 0;  // M
    }
}

void GpgxBackend::trackZ80Call(uint16_t pc)
{
    const uint8_t op = z80ByteAt(pc);
    bool call = false, ret = false;
    switch (op) {
    case 0xCD:                                          // CALL nn
        call = true; break;
    case 0xDC: case 0xFC: case 0xD4: case 0xC4:         // CALL cc,nn
    case 0xF4: case 0xEC: case 0xE4: case 0xCC:
        call = z80CondTrue(op, Z80.af.w.l); break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:         // RST — always taken
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        call = true; break;
    case 0xC9:                                          // RET
        ret = true; break;
    case 0xD8: case 0xF8: case 0xD0: case 0xC0:         // RET cc
    case 0xF0: case 0xE8: case 0xE0: case 0xC8:
        ret = z80CondTrue(op, Z80.af.w.l); break;
    case 0xED:                                          // reti / retn
        if (z80ByteAt(uint16_t(pc + 1)) == 0x4D || z80ByteAt(uint16_t(pc + 1)) == 0x45)
            ret = true;
        break;
    default: break;
    }
    if (!call && !ret) return;

    std::lock_guard<std::mutex> lk(callstackMutex_);
    if (call) { if (callstackZ80_.size() < 256) callstackZ80_.push_back(pc); }
    else if (!callstackZ80_.empty()) callstackZ80_.pop_back();
}

void GpgxBackend::armStepOverZ80()
{
    const uint16_t pc = Z80.pc.w.l;
    const uint8_t  op = z80ByteAt(pc);
    int len = 0;
    switch (op) {
    case 0xCD: case 0xDC: case 0xFC: case 0xD4: case 0xC4:
    case 0xF4: case 0xEC: case 0xE4: case 0xCC:
        len = 3; break;                                   // CALL nn
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        len = 1; break;                                   // RST n
    case 0xED:
        if (z80ByteAt(uint16_t(pc + 1)) == 0xB0) len = 2; // LDIR: stepping it is a loop
        break;
    default: break;
    }
    if (len == 0) { armStepIntoLocked(Cpu::Z80); return; }  // nothing to step over

    stepOverAddrZ80_.store(int(uint16_t(pc + len)));
}

void GpgxBackend::onZ80Exec(uint32_t pc)
{
    // A HALT re-executes its own address; without this a breakpoint on it would
    // fire again the moment we resume, and never let go.
    const int skip = z80ResumeSkip_.load();
    if (skip >= 0) {
        if (uint32_t(skip) == pc) return;
        z80ResumeSkip_.store(-1);
    }

    lastPcZ80_ = pc;
    trackZ80Call(uint16_t(pc));

    bool brk = stepIntoZ80_.exchange(false);
    if (!brk) {
        const int so = stepOverAddrZ80_.load();
        if (so >= 0 && uint32_t(so) == pc) { stepOverAddrZ80_.store(-1); brk = true; }
    }
    int hit = -1;
    if (!brk) { hit = matchBreakpoint(HOOK_Z80_E, pc); brk = hit >= 0; }
    if (brk) {
        z80ResumeSkip_.store(int(pc));
        // Usually the 68000 is between instructions here, except when one of
        // its writes (BUSREQ/RESET) ran the Z80 to catch up.
        stopInInstr_ = m68kMidInstruction();
        stopMidInstr_ = stopInInstr_;
        firePause(pc, Cpu::Z80, hit, hit >= 0 ? StopReason::Breakpoint : StopReason::Step);
    }
}

// ---------------------------------------------------------------------------
// Callstack — the same heuristic the original Gens used: push the address of
// each jsr/bsr, pop on rts/rte. Conditional returns are popped unconditionally,
// so it can drift; it is a navigation aid, not ground truth.
// ---------------------------------------------------------------------------
uint16_t GpgxBackend::opcodeAt(uint32_t pc) const
{
    const cpu_memory_map* map = &m68k.memory_map[(pc >> 16) & 0xFF];
    if (!map->base) return 0;
    // storage is word-swapped on LE hosts: logical hi byte lives at addr^1
    return uint16_t((map->base[(pc & 0xFFFF) ^ 1] << 8) | map->base[((pc + 1) & 0xFFFF) ^ 1]);
}

void GpgxBackend::trackCall(uint32_t pc)
{
    const uint16_t opc = opcodeAt(pc);
    const bool isCall   = ((opc & 0xFFC0) == 0x4E80)      // jsr
                       || ((opc & 0xFF00) == 0x6100);     // bsr
    const bool isReturn = (opc == 0x4E75) || (opc == 0x4E73);  // rts / rte
    if (!isCall && !isReturn) return;

    std::lock_guard<std::mutex> lk(callstackMutex_);
    if (isCall) {
        if (callstack_.size() < 256) callstack_.push_back(pc);   // runaway guard
    } else if (!callstack_.empty()) {
        callstack_.pop_back();
    }
}

std::vector<uint32_t> GpgxBackend::getCallstack(Cpu cpu)
{
    std::lock_guard<std::mutex> lk(callstackMutex_);
    return (cpu == Cpu::Z80) ? callstackZ80_ : callstack_;
}
