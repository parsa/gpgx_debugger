#pragma once
#include "IDebugBackend.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

class GpgxBackend;

// Where a run should stop on its own (GpgxBackend::armRun). Any combination;
// the first condition met stops it, with the matching StopReason.
struct RunLimits {
    // Return: stop when PC == returnAddr AND A7 > entrySp (the frame that held
    // the return address has been popped; a deeper re-entry does not count).
    bool     toReturn   = false;
    uint32_t returnAddr = 0;
    uint32_t entrySp    = 0;
    int64_t  stopAddr   = -1;       // StopAddress when PC reaches it; -1 = none
    uint64_t maxInstructions = 0;   // Budget after this many retire; 0 = none
    uint64_t maxFrames  = 0;        // Budget after this many frames; 0 = none

    // Return from the routine the CPU is standing at the entry of: the return
    // address is at (A7), and A7 is the entry stack pointer.
    static RunLimits toReturnFromHere(GpgxBackend& b);
    bool any() const { return toReturn || stopAddr >= 0 || maxInstructions || maxFrames; }
};

class GpgxBackend final : public IDebugBackend {
public:
    GpgxBackend();
    ~GpgxBackend() override;

    M68kRegs   getM68kRegs() override;
    void       setM68kRegs(const M68kRegs&) override;
    Z80Regs    getZ80Regs() override;
    void       setZ80Regs(const Z80Regs&) override;
    VdpState   getVdpState() override;
    void       setVdpReg(int idx, uint8_t value) override;
    SoundState getSoundState() override;

    std::vector<uint8_t> readMemory(uint32_t addr, uint32_t size) override;
    bool writeMemory(uint32_t addr, const uint8_t* data, uint32_t size) override;
    std::vector<uint8_t> readZ80Memory(uint16_t addr, uint16_t size) override;

    bool saveState(const char* path) override;
    bool loadState(const char* path) override;
    bool runSafely(const std::function<void()>& fn) override;

    // A host that owns the emulation thread (EmuHost) installs its queue here;
    // without one runSafely() falls back to pausing around the call.
    using SafeExec = std::function<bool(const std::function<void()>&)>;
    void setSafeExecutor(SafeExec e) { safeExec_ = std::move(e); }

    void     setPad(int port, uint16_t buttons) override;
    uint16_t getPad(int port) override;

    std::vector<MemRegion> getMemRegions() override;
    std::vector<uint8_t>   readRegion(int id, uint32_t off, uint32_t size) override;
    bool                   writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size) override;

    int  addBreakpoint(const Breakpoint&) override;
    void removeBreakpoint(int id) override;
    bool updateBreakpoint(const Breakpoint&) override;
    void clearBreakpoints() override;
    std::vector<Breakpoint> getBreakpoints() override;
    std::vector<uint32_t>   getCallstack(Cpu cpu) override;
    void setConditionEvaluator(ConditionEval e) override { condEval_ = std::move(e); }

    void pause() override;
    void resume() override;
    void stepInto(Cpu cpu) override;
    void stepOver(Cpu cpu) override;
    bool isPaused() const override { return paused_.load(); }

    void onPaused(PauseCb cb)  override { pauseCb_  = std::move(cb); }
    void onResumed(ResumeCb cb) override { resumeCb_ = std::move(cb); }

    bool loadRom(const char* path) override;
    bool isRunning() const override { return running_.load(); }

    void onCpuHook(int type, int width, uint32_t addr, uint32_t value);

    // Identity of the loaded game, for clients that must pick between several
    // running emulators. Empty until a ROM is loaded.
    SessionInfo sessionInfo() const;

    // Codemap ("changed"): executed pc -> predecessor pc, accumulated during
    // execution, drained (and cleared) atomically with each pause. Used by the
    // IDA host for auto_make_code() on everything executed.
    std::map<uint32_t, uint32_t> takeCodemap();

    // Called repeatedly while the emulation thread is blocked in a pause. The
    // host installs a pump that drains pending debug commands so run-control
    // (resume/step) and reads can be serviced mid-pause — the nested-loop model
    // of the original Gens debugger.
    void setPausePump(std::function<void()> pump) { pausePump_ = std::move(pump); }

    // Abandon run control so the emulation thread can leave. Resuming alone is
    // not enough to shut down: the very next instruction can hit the same
    // breakpoint — or a still-armed step — and park the thread again, leaving
    // the host waiting on a join that never comes. After this, pauses stop
    // happening at all until the next loadRom.
    void abortRunControl();

    // Which breakpoint caused the current stop, or -1 for a step, an explicit
    // pause, or a stop that no breakpoint explains. Read after a pause event.
    int lastStopBreakpoint() const { return lastBpId_.load(); }
    // 68000 exceptions. Every one is reported through the callback (on the
    // emulation thread, before its vector is read); those whose vector bit is
    // set in the catch mask also stop, with StopReason::Exception, on the
    // faulting instruction. Bit n = vector n. Interrupts are not reported.
    using ExceptionCb = std::function<void(const ExceptionInfo&)>;
    void onException(ExceptionCb cb) { excCb_ = std::move(cb); }
    void     setExceptionCatch(uint64_t vectors) { excCatch_.store(vectors); }
    uint64_t exceptionCatch() const { return excCatch_.load(); }
    ExceptionInfo lastException() const;
    // Arm run limits for the next resume (replacing any armed ones). Call while
    // paused. They disarm when one fires, and on disarmRun().
    void armRun(const RunLimits& lim);
    void disarmRun() { runActive_.store(false); }
    // The host calls this after every frame (frame budgets).
    void onFrameEnd();

    // The access behind the current Watchpoint stop.
    AccessInfo lastAccess() const;

    // Access log: every memory/I/O access matching cfg.ranges (68000 bus, Z80
    // bus, VDP memories), with the instruction that made it. Starting clears
    // it. Bounded by cfg.capacity; further matches only count as dropped.
    void      startAccessLog(const AccessLogConfig& cfg);
    void      stopAccessLog();
    bool      accessLogEnabled() const { return logOn_.load(std::memory_order_relaxed); }
    AccessLog accessLog() const;

    // Why the current stop happened. Read after a pause event.
    StopReason lastStopReason() const { return StopReason(lastReason_.load()); }

    // Pause at the next opportunity, reporting `why` (pause() = Pause).
    void requestPause(StopReason why);

    // Called by the host between frames. A 68000 that is halted or waiting in
    // STOP executes no instructions, so a pause or step request would never
    // reach the instruction hook; this delivers it as a Halted/Stopped stop.
    void serviceStoppedCpu();

private:
    void firePause(uint32_t pc, Cpu cpu, int bpId, StopReason why);
    // width = bytes touched by the access, so a breakpoint inside a word or
    // longword access still matches.
    int  matchBreakpoint(int type, uint32_t addr, int width = 1);   // id, or -1
    void trackCall(uint32_t pc);          // maintain callstack_ from the opcode at pc
    uint16_t opcodeAt(uint32_t pc) const;
    void     onZ80Exec(uint32_t pc);
    void     onM68kException(unsigned vector);
    void     stepOverZ80();
    void     trackZ80Call(uint16_t pc);
    uint8_t  z80ByteAt(uint16_t pc) const;

    PauseCb  pauseCb_;
    ResumeCb resumeCb_;
    std::function<void()> pausePump_;
    SafeExec safeExec_;

    std::atomic<bool> running_  {false};
    std::atomic<bool> paused_   {false};
    std::atomic<bool> abandoned_{false};   // shutting down: never park again
    std::atomic<int>  lastBpId_ {-1};      // breakpoint behind the current stop
    std::atomic<uint8_t> lastReason_ {0};  // StopReason of the current stop
    std::atomic<uint8_t> pauseReason_ {0}; // pending pause request (StopReason), 0 = none
    std::atomic<bool> stepInto_ {false};
    std::atomic<int>  stepOverAddr_ {-1};
    // The Z80 needs its own: it retires thousands of instructions per frame,
    // so a step meant for the 68000 would be consumed by the wrong CPU.
    std::atomic<bool> stepIntoZ80_ {false};
    std::atomic<int>  stepOverAddrZ80_ {-1};
    // A HALT re-executes the same address forever, so a breakpoint there would
    // retrigger the instant it resumes. Skip one hit at the address we stopped.
    std::atomic<int>  z80ResumeSkip_ {-1};

    std::mutex bpMutex_;
    std::vector<Breakpoint> breakpoints_;
    // Read on every instruction of both CPUs, so the common "no breakpoints"
    // case must not touch the mutex at all.
    std::atomic<int> bpCount_ {0};
    int nextBpId_ = 1;
    uint32_t lastPc_ = 0;
    // 68000 instructions retired (emulation thread only), and the count at the
    // previous frame end: equal means the CPU executed nothing for a frame.
    uint64_t execCount_ = 0;
    uint64_t execCountAtFrame_ = 0;
    // Set by a stop delivered between frames: the next instruction hook is the
    // instruction the CPU is standing on, and must execute rather than stop.
    bool standingOnNext_ = false;

    std::mutex codemapMutex_;
    std::map<uint32_t, uint32_t> codemap_;

    ConditionEval condEval_;
    ExceptionCb   excCb_;
    std::atomic<uint64_t> excCatch_ {0};
    mutable std::mutex    excMx_;
    ExceptionInfo         lastExc_;
    AccessInfo            lastAccess_;   // guarded by excMx_

    // Armed run limits: written while paused (armRun), read by the emulation
    // thread; runActive_ publishes them.
    std::atomic<bool>     runActive_ {false};
    RunLimits             run_;
    uint64_t              runStartExec_ = 0;
    uint64_t              runFrames_ = 0;

    std::atomic<bool>     logOn_ {false};
    mutable std::mutex    logMx_;
    AccessLogConfig       logCfg_;       // guarded by logMx_
    AccessLog             log_;          // guarded by logMx_
    void logAccess(uint32_t pc, Cpu cpu, AccessSpace space, bool write,
                   int width, uint32_t addr, uint32_t value);
    void watchHit(int bpId, uint32_t pc, Cpu cpu, AccessSpace space, bool write,
                  int width, uint32_t addr, uint32_t value);
    std::mutex            callstackMutex_;
    std::vector<uint32_t> callstack_;
    std::vector<uint32_t> callstackZ80_;
    uint32_t              lastPcZ80_ = 0;
};

extern GpgxBackend* g_gpgxBackend;
