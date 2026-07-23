#include "EmuHost.h"

#include <chrono>

#include <gx/gx.hpp>

extern "C" {
#include <shared.h>       // uint8/uint16/uint32 macros — must be first
#include <system.h>       // system_frame_gen, vdp_pal
#include <vdp_ctrl.h>
}

EmuHost::EmuHost() = default;

EmuHost::~EmuHost()
{
    stop();
}

bool EmuHost::start(const std::string& romPath)
{
    stop();
    stopFlag_.store(false);

    // Load synchronously so the caller learns immediately whether the ROM is
    // valid; the thread only runs frames.
    if (!backend_.loadRom(romPath.c_str()))
        return false;

    // While paused, service commands so run-control/reads work mid-pause.
    backend_.setPausePump([this] {
        drainCommands();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    backend_.onPaused([this](uint32_t pc) {
        DebugEvent ev;
        ev.type = DebugEvent::Type::Paused;
        ev.pc = pc;
        ev.changed = backend_.takeCodemap();
        emitEvent(ev);
    });
    backend_.onResumed([this] {
        DebugEvent ev;
        ev.type = DebugEvent::Type::Resumed;
        emitEvent(ev);
    });

    running_.store(true);
    thread_ = std::thread(&EmuHost::run, this, romPath);

    DebugEvent started;
    started.type = DebugEvent::Type::Started;
    started.pc = backend_.getM68kRegs().pc;
    emitEvent(started);
    return true;
}

void EmuHost::stop()
{
    if (!thread_.joinable()) { running_.store(false); return; }
    stopFlag_.store(true);
    backend_.resume();            // unblock a pause spin so the thread can exit
    thread_.join();
    running_.store(false);

    DebugEvent stopped;
    stopped.type = DebugEvent::Type::Stopped;
    stopped.changed = backend_.takeCodemap();
    emitEvent(stopped);
}

void EmuHost::run(std::string /*romPath*/)
{
    while (!stopFlag_.load()) {
        drainCommands();

        system_frame_gen(0);      // one field/frame; blocks in firePause on bp

        if (frameSink_) {
            const t_bitmap& bm = ::bitmap;
            frameSink_(bm.data, bm.width, bm.height, bm.pitch);
        }
        // No frame pacing here: a debugger host runs as fast as the machine
        // (IDA gates progress via pauses). A GUI host that wants 60 fps should
        // pace in its FrameSink or drive frames itself.
    }
    gx::shutdown();
}

void EmuHost::drainCommands()
{
    if (!transport_) return;
    DebugCommand cmd;
    while (transport_->recvCommand(cmd))
        dispatch(cmd);
}

void EmuHost::dispatch(const DebugCommand& cmd)
{
    using Op = DebugCommand::Op;
    switch (cmd.op) {
    case Op::Pause:    backend_.pause();    break;
    case Op::Resume:   backend_.resume();   break;
    case Op::StepInto: backend_.stepInto(); break;
    case Op::StepOver: backend_.stepOver(); break;
    case Op::WriteMemory:
        if (!cmd.data.empty())
            backend_.writeMemory(cmd.addr, cmd.data.data(), (uint32_t)cmd.data.size());
        break;
    case Op::SetVdpReg:
        backend_.setVdpReg(cmd.index, (uint8_t)cmd.value);
        break;
    case Op::AddBreakpoint: {
        Breakpoint bp;
        bp.type   = (BpType)cmd.bpType;
        bp.is_vdp = cmd.bpIsVdp != 0;
        bp.start  = cmd.bpStart;
        bp.end    = cmd.bpEnd;
        bp.condition = cmd.bpCondition;
        backend_.addBreakpoint(bp);
        break;
    }
    case Op::RemoveBreakpoint: backend_.removeBreakpoint(cmd.index); break;
    case Op::ClearBreakpoints: backend_.clearBreakpoints();          break;
    case Op::ExitEmulation:    stopFlag_.store(true); backend_.resume(); break;
    // Read-style ops (GetM68kRegs/GetVdpState/ReadMemory/...) are serviced by
    // in-proc hosts via backend() directly (valid while paused). A separate
    // process transport will add a reply channel; not needed for static link.
    default: break;
    }
}

void EmuHost::emitEvent(const DebugEvent& ev)
{
    if (transport_) transport_->sendEvent(ev);
    if (eventSink_) eventSink_(ev);
}
