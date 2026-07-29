#pragma once
#include "IDebugBackend.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

class GpgxBackend final : public IDebugBackend {
public:
    GpgxBackend();
    ~GpgxBackend() override;

    M68kRegs   getM68kRegs() override;
    void       setM68kRegs(const M68kRegs&) override;
    Z80Regs    getZ80Regs() override;
    VdpState   getVdpState() override;
    void       setVdpReg(int idx, uint8_t value) override;
    SoundState getSoundState() override;

    std::vector<uint8_t> readMemory(uint32_t addr, uint32_t size) override;
    bool writeMemory(uint32_t addr, const uint8_t* data, uint32_t size) override;
    std::vector<uint8_t> readZ80Memory(uint16_t addr, uint16_t size) override;

    void     setPad(int port, uint16_t buttons) override;
    uint16_t getPad(int port) override;

    std::vector<MemRegion> getMemRegions() override;
    std::vector<uint8_t>   readRegion(int id, uint32_t off, uint32_t size) override;
    bool                   writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size) override;

    int  addBreakpoint(const Breakpoint&) override;
    void removeBreakpoint(int id) override;
    void clearBreakpoints() override;
    std::vector<Breakpoint> getBreakpoints() override;

    void pause() override;
    void resume() override;
    void stepInto() override;
    void stepOver() override;
    bool isPaused() const override { return paused_.load(); }

    void onPaused(PauseCb cb)  override { pauseCb_  = std::move(cb); }
    void onResumed(ResumeCb cb) override { resumeCb_ = std::move(cb); }

    bool loadRom(const char* path) override;
    bool isRunning() const override { return running_.load(); }

    void onCpuHook(int type, int width, uint32_t addr, uint32_t value);

    // Codemap ("changed"): executed pc -> predecessor pc, accumulated during
    // execution, drained (and cleared) atomically with each pause. Used by the
    // IDA host for auto_make_code() on everything executed.
    std::map<uint32_t, uint32_t> takeCodemap();

    // Called repeatedly while the emulation thread is blocked in a pause. The
    // host installs a pump that drains pending debug commands so run-control
    // (resume/step) and reads can be serviced mid-pause — the nested-loop model
    // of the original Gens debugger.
    void setPausePump(std::function<void()> pump) { pausePump_ = std::move(pump); }

private:
    void firePause(uint32_t pc);
    bool matchBreakpoint(int type, uint32_t addr);

    PauseCb  pauseCb_;
    ResumeCb resumeCb_;
    std::function<void()> pausePump_;

    std::atomic<bool> running_  {false};
    std::atomic<bool> paused_   {false};
    std::atomic<bool> stepInto_ {false};
    std::atomic<int>  stepOverAddr_ {-1};

    std::mutex bpMutex_;
    std::vector<Breakpoint> breakpoints_;
    int nextBpId_ = 1;
    uint32_t lastPc_ = 0;

    std::mutex codemapMutex_;
    std::map<uint32_t, uint32_t> codemap_;
};

extern GpgxBackend* g_gpgxBackend;
