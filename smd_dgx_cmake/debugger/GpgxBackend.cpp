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

static void cpuHookShim(hook_type_t type, int width, unsigned int addr, unsigned int value)
{
    if (g_gpgxBackend)
        g_gpgxBackend->onCpuHook(static_cast<int>(type), width, addr, value);
}

GpgxBackend::GpgxBackend()  { g_gpgxBackend = this; set_cpu_hook(cpuHookShim); }
GpgxBackend::~GpgxBackend() { set_cpu_hook(nullptr); g_gpgxBackend = nullptr; }

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
    for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i), r.d[i]);
    for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i), r.a[i]);
    m68k_set_reg(M68K_REG_PC,  r.pc);
    m68k_set_reg(M68K_REG_SR,  r.sr);
    m68k_set_reg(M68K_REG_USP, r.usp);
    m68k_set_reg(M68K_REG_ISP, r.isp);
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
    return r;
}

VdpState GpgxBackend::getVdpState()
{
    VdpState v{};
    std::memcpy(v.reg, reg, sizeof(v.reg));
    v.status   = ::status;
    v.dma_len  = (reg[20] & 0xFF) | ((reg[19] & 0xFF) << 8);
    v.dma_src  = (reg[21] & 0xFF) | ((reg[22] & 0xFF) << 8) | ((reg[23] & 0x7F) << 16);
    v.dma_type = ::dma_type;
    v.vram  = ::vram;
    v.cram  = ::cram;
    v.vsram = ::vsram;
    v.sat   = ::sat;
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

std::vector<uint8_t> GpgxBackend::readZ80Memory(uint16_t addr, uint16_t size)
{
    std::vector<uint8_t> buf(size, 0xFF);
    for (uint16_t i = 0; i < size; ++i)
        if ((uint16_t)(addr + i) < 0x2000) buf[i] = ::zram[addr + i];
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
    return b.id;
}
void GpgxBackend::removeBreakpoint(int id)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    breakpoints_.erase(std::remove_if(breakpoints_.begin(), breakpoints_.end(),
        [id](const Breakpoint& b){ return b.id == id; }), breakpoints_.end());
}
void GpgxBackend::clearBreakpoints() { std::lock_guard<std::mutex> lk(bpMutex_); breakpoints_.clear(); }
std::vector<Breakpoint> GpgxBackend::getBreakpoints() { std::lock_guard<std::mutex> lk(bpMutex_); return breakpoints_; }

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
void GpgxBackend::pause()   { stepInto_.store(true); }
void GpgxBackend::resume()  { paused_.store(false); if (resumeCb_) resumeCb_(); }
void GpgxBackend::stepInto(){ paused_.store(false); stepInto_.store(true); }

void GpgxBackend::stepOver()
{
    uint32_t pc = m68k_get_reg(M68K_REG_PC);
    const cpu_memory_map* map = &m68k.memory_map[(pc >> 16) & 0xFF];
    uint16_t opc = 0;
    // storage is word-swapped on LE hosts: logical hi byte lives at addr^1
    if (map->base) opc = (static_cast<uint16_t>(map->base[(pc & 0xFFFF) ^ 1]) << 8) | map->base[((pc+1) & 0xFFFF) ^ 1];
    if ((opc & 0xFFC0) == 0x4E80) {
        int off = 1, mod = (opc >> 3) & 7;
        if (mod == 5) off++; else if (mod == 7) { off += (opc & 7) == 1 ? 2 : 1; }
        stepOverAddr_.store(static_cast<int>(pc + off * 2));
    } else if ((opc & 0xFF00) == 0x6100) {
        int off = (opc & 0xFF) == 0 ? 2 : (opc & 0xFF) == 0xFF ? 3 : 1;
        stepOverAddr_.store(static_cast<int>(pc + off * 2));
    } else { stepInto_.store(true); return; }
    paused_.store(false); if (resumeCb_) resumeCb_();
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

    bool ok = gx::load_rom(path);
    if (ok) running_.store(true);
    return ok;
}

// ---------------------------------------------------------------------------
// CPU hook
// ---------------------------------------------------------------------------
void GpgxBackend::onCpuHook(int type, int /*width*/, uint32_t addr, uint32_t /*value*/)
{
    if (type & HOOK_M68K_E) {
        // codemap: record predecessor of every executed ROM/RAM address
        if (lastPc_ && addr && addr < MAXROMSIZE) {
            std::lock_guard<std::mutex> lk(codemapMutex_);
            codemap_[addr] = lastPc_;
        }
        lastPc_ = addr;
        bool brk = stepInto_.exchange(false);
        if (!brk) { int so = stepOverAddr_.load(); if (so >= 0 && (uint32_t)so == addr) { stepOverAddr_.store(-1); brk = true; } }
        if (!brk) brk = matchBreakpoint(type, addr);
        if (brk) firePause(addr);
    } else if (type & (HOOK_M68K_R | HOOK_M68K_W)) {
        if (matchBreakpoint(type, addr)) firePause(lastPc_);
    }
}

std::map<uint32_t, uint32_t> GpgxBackend::takeCodemap()
{
    std::lock_guard<std::mutex> lk(codemapMutex_);
    std::map<uint32_t, uint32_t> out;
    out.swap(codemap_);
    return out;
}

void GpgxBackend::firePause(uint32_t pc)
{
    paused_.store(true);
    if (pauseCb_) pauseCb_(pc);
    // Block the emulation thread until resumed. While blocked, pump host
    // commands (resume/step/read) so run-control can proceed — nested-loop
    // model of the original Gens debugger.
    while (paused_.load()) {
        if (pausePump_) pausePump_();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool GpgxBackend::matchBreakpoint(int type, uint32_t addr)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    for (const auto& bp : breakpoints_) {
        if (!bp.enabled) continue;
        if (bp.type == BpType::PC    && (type & HOOK_M68K_E) && addr >= bp.start && addr <= bp.end) return true;
        if (bp.type == BpType::Read  && (type & HOOK_M68K_R) && addr >= bp.start && addr <= bp.end) return true;
        if (bp.type == BpType::Write && (type & HOOK_M68K_W) && addr >= bp.start && addr <= bp.end) return true;
    }
    return false;
}
