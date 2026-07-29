#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "DebugState.h"

// Pure C++ abstract backend – no Qt dependency.
// GpgxBackend implements this in-process; an IDA-plugin host or an IPC proxy
// can implement the same surface. Views depend ONLY on this interface and
// DebugState.h so they can be re-hosted (e.g. embedded into IDA, which is a
// Qt application) without changes.
class IDebugBackend {
public:
    virtual ~IDebugBackend() = default;

    // CPU / VDP / sound state
    virtual M68kRegs   getM68kRegs() = 0;
    virtual void       setM68kRegs(const M68kRegs&) = 0;
    virtual Z80Regs    getZ80Regs() = 0;
    virtual VdpState   getVdpState() = 0;
    virtual void       setVdpReg(int idx, uint8_t value) = 0;   // raw shadow write, Gens semantics
    virtual SoundState getSoundState() = 0;

    // Flat 68K-bus access (24-bit addresses, logical byte order)
    virtual std::vector<uint8_t> readMemory(uint32_t addr, uint32_t size) = 0;
    virtual bool                 writeMemory(uint32_t addr, const uint8_t* data, uint32_t size) = 0;
    virtual std::vector<uint8_t> readZ80Memory(uint16_t addr, uint16_t size) = 0;

    // Region-based access (hex editor / RAM tools). Logical byte order.
    virtual std::vector<MemRegion>  getMemRegions() = 0;
    virtual std::vector<uint8_t>    readRegion(int id, uint32_t off, uint32_t size) = 0;
    virtual bool                    writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size) = 0;

    // Save states. These snapshot/replace the whole machine, so they MUST run
    // on the emulation thread (EmuHost::post) or while it is paused — calling
    // them alongside a running core yields a torn state.
    virtual bool saveState(const char* path) = 0;
    virtual bool loadState(const char* path) = 0;

    // Controller input: a PadButton mask per port. Safe to call from any
    // thread — the core reads the pad once per frame.
    virtual void     setPad(int port, uint16_t buttons) = 0;
    virtual uint16_t getPad(int port) = 0;

    // Breakpoints
    virtual int  addBreakpoint(const Breakpoint&) = 0;
    virtual void removeBreakpoint(int id) = 0;
    virtual void clearBreakpoints() = 0;
    virtual std::vector<Breakpoint> getBreakpoints() = 0;

    // Run control
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stepInto() = 0;
    virtual void stepOver() = 0;
    virtual bool isPaused() const = 0;

    using PauseCb  = std::function<void(uint32_t pc)>;
    using ResumeCb = std::function<void()>;
    virtual void onPaused(PauseCb)  = 0;
    virtual void onResumed(ResumeCb) = 0;

    virtual bool loadRom(const char* path) = 0;
    virtual bool isRunning() const = 0;
};
