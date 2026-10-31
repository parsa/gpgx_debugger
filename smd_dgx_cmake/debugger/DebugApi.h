#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "DebugState.h"   // Cpu

// ---------------------------------------------------------------------------
// Transport-agnostic debug protocol.
//
// This is the minimal command/event surface distilled from the old
// smd_ida_tools2 protobuf/gRPC protocol (DbgServer/DbgClient services).
// The protobuf layer is gone; the same payloads can be carried by:
//   - InProcTransport (std::deque + mutex)  — static linking: emulator lives
//     inside the client process (e.g. the IDA plugin) on its own thread;
//   - a future shared-memory ring            — separate processes;
//   - optionally a protobuf adapter          — legacy Gensida compatibility.
//
// Payloads deliberately keep flat/simple types so they can later be
// serialized for IPC without redesign. `changed` (the codemap) is the one
// variable-size payload: executed-pc -> predecessor-pc, flushed and cleared
// atomically with every pause/stop event, exactly like the Gens original.
// ---------------------------------------------------------------------------

struct DebugCommand {
    enum class Op : uint8_t {
        Pause, Resume, StepInto, StepOver,
        ReadMemory, WriteMemory,
        GetM68kRegs, SetM68kRegs, GetZ80Regs,
        GetVdpState, SetVdpReg, GetSoundState,
        AddBreakpoint, RemoveBreakpoint, ClearBreakpoints,
        GetCallstack,
        LoadRom, ExitEmulation,
    };
    Op       op{};
    Cpu      cpu = Cpu::M68K;   // StepInto/StepOver/AddBreakpoint target
    uint32_t addr = 0;      // ReadMemory/WriteMemory
    uint32_t size = 0;
    int      index = 0;     // SetVdpReg reg index / RemoveBreakpoint id
    uint32_t value = 0;     // SetVdpReg value
    std::vector<uint8_t> data;   // WriteMemory payload / LoadRom path bytes
    // AddBreakpoint payload (flat, mirrors DebugState.h Breakpoint)
    uint8_t  bpType = 0;
    uint8_t  bpIsVdp = 0;
    uint32_t bpStart = 0, bpEnd = 0;
    std::string bpCondition;
};

struct DebugEvent {
    enum class Type : uint8_t {
        Started,        // emulation started (ROM loaded)
        Paused,         // pc valid; changed = codemap since last event
        Resumed,
        Stopped,        // emulator exiting; changed flushed one last time
        Exception,      // a 68000 exception was raised (exc); not a stop by itself
    };
    Type     type{};
    Cpu      cpu = Cpu::M68K;   // which processor stopped
    uint32_t pc  = 0;
    int      bpId = -1;         // breakpoint that caused it, or -1
    StopReason reason = StopReason::None;   // Paused: why
    ExceptionInfo exc;          // Exception, or Paused with reason Exception
    AccessInfo access;          // Paused with reason Watchpoint
    uint64_t stopSeq = 0;       // monotonic; distinguishes a new stop from a seen one
    std::map<uint32_t, uint32_t> changed;   // executed pc -> predecessor pc
};

// A Paused event's cause as space-separated key=value pairs, starting with
// "reason=<name>". The same text is appended to bridge replies and returned by
// IDA's smd_dgx_stop(), so every client parses one format.
// Upper-case hex, zero-padded to `digits`. (No printf family: IDA's headers
// poison it for code that includes them, and this header is shared.)
inline std::string hexDigits(uint32_t v, int digits)
{
    std::string s(size_t(digits), '0');
    for (int i = digits - 1; i >= 0; --i, v >>= 4) s[size_t(i)] = "0123456789ABCDEF"[v & 15];
    return s;
}

inline std::string describeException(const ExceptionInfo& x)
{
    std::string s = "vector=" + std::to_string(unsigned(x.vector))
                  + " name=" + exceptionName(x.vector)
                  + " fault_pc=" + hexDigits(x.pc & 0xFFFFFF, 6)
                  + " frame_pc=" + hexDigits(x.framePc & 0xFFFFFF, 6)
                  + " sr=" + hexDigits(x.sr, 4);
    if (x.access != ExcAccess::None)
        s += std::string(" access=") + excAccessName(x.access)
           + " addr=" + hexDigits(x.address & 0xFFFFFF, 6);
    return s;
}

inline std::string describeStop(const DebugEvent& ev)
{
    if (ev.type == DebugEvent::Type::Exception) return describeException(ev.exc);
    if (ev.type != DebugEvent::Type::Paused) return {};
    std::string s = "reason=";
    s += stopReasonName(ev.reason);
    if (ev.reason == StopReason::Exception) s += " " + describeException(ev.exc);
    if (ev.reason == StopReason::Watchpoint) {
        const AccessInfo& a = ev.access;
        const char* space = a.space == AccessSpace::Z80 ? "z80" : a.space == AccessSpace::Vdp ? "vdp" : "m68k";
        s += std::string(" access=") + (a.write ? "write" : "read") + " space=" + space
           + " addr=" + hexDigits(a.address & 0xFFFFFF, 6)
           + " size=" + std::to_string(unsigned(a.size))
           + " value=" + hexDigits(a.value, a.size >= 4 ? 8 : a.size == 2 ? 4 : 2);
    }
    return s;
}

// Exception catch mask from text: none | faults (vectors 2-11, 14, 15) |
// all (every reported vector) | hex mask (bit n = vector n, optional 0x).
inline bool parseCatchSpec(const std::string& spec, uint64_t& mask)
{
    if (spec == "none")   { mask = 0; return true; }
    if (spec == "faults") { mask = 0xCFFCull; return true; }
    if (spec == "all")    { mask = ~0ull & ~0xFF000003ull; return true; }   // not 0, 1, 24-31
    const char* s = spec.c_str();
    if (spec.rfind("0x", 0) == 0 || spec.rfind("0X", 0) == 0) s += 2;
    if (!*s) return false;
    uint64_t v = 0;
    for (; *s; ++s) {
        const char c = *s;
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0 || (v >> 60)) return false;
        v = (v << 4) | uint64_t(d);
    }
    mask = v;
    return true;
}

// Bidirectional transport endpoint. The emulator side sends events and
// receives commands; the client side (UI / IDA) does the reverse.
class IDebugTransport {
public:
    virtual ~IDebugTransport() = default;

    // emulator side
    virtual bool sendEvent(const DebugEvent&) = 0;
    virtual bool recvCommand(DebugCommand&) = 0;      // non-blocking; false = empty

    // client side
    virtual bool sendCommand(const DebugCommand&) = 0;
    virtual bool recvEvent(DebugEvent&) = 0;          // non-blocking; false = empty

    virtual bool isConnected() const = 0;
};
