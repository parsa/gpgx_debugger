#pragma once
#include "GpgxBackend.h"
#include "DebugApi.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Headless emulator host: owns a GpgxBackend, runs the emulator on its own
// std::thread, and bridges it to a command/event transport. No Qt, no SDL, no
// audio/video — the pixel/sound sinks are optional callbacks.
//
// This is the reusable "server" glue shared by every host:
//   - standalone Qt app  : may drive the backend directly OR via a transport;
//   - IDA plugin (static) : holds the EmuHost in-process, calls backend()
//                           directly for synchronous reads (valid while paused,
//                           the emulation thread is blocked), and consumes
//                           DebugEvents to build IDA debug_event_t's.
//
// Run-control commands may arrive either by direct backend() calls (in-proc) or
// through the transport queue; both are serviced on the emulation thread, so
// the emulator core is only ever touched from one thread.
class EmuHost {
public:
    // Optional sinks. FrameSink receives the rendered framebuffer each frame
    // (RGB565, gpgx t_bitmap geometry); omit for a truly headless run.
    using FrameSink = std::function<void(const uint8_t* data, int width, int height, int pitch)>;
    using EventSink = std::function<void(const DebugEvent&)>;

    EmuHost();
    ~EmuHost();

    // Attach a transport (e.g. InProcTransport). The host will drain commands
    // from it (between frames and during pauses) and post events to it. May be
    // null for a pure direct-access host.
    void setTransport(IDebugTransport* t) { transport_ = t; }
    void setFrameSink(FrameSink s) { frameSink_ = std::move(s); }
    void setEventSink(EventSink s) { eventSink_ = std::move(s); }

    // Direct access for in-proc hosts (synchronous reads/writes; call while
    // paused). Never null after construction.
    IDebugBackend* backend() { return &backend_; }
    GpgxBackend*   gpgx()    { return &backend_; }

    // Load a ROM and start the emulation thread. Returns false if the ROM
    // failed to load (no thread is started in that case).
    bool start(const std::string& romPath);

    // Signal the emulation thread to exit and join it.
    void stop();

    bool isRunning() const { return running_.load(); }

    // Execute a single command on the calling thread (used by the pump and by
    // hosts that want to dispatch synchronously). Returns any reply data for
    // read-style commands via the out params; control commands ignore them.
    void dispatch(const DebugCommand& cmd);

private:
    void run(std::string romPath);
    void drainCommands();
    void emitEvent(const DebugEvent& ev);   // not `emit`: clashes with Qt's macro

    GpgxBackend       backend_;
    IDebugTransport*  transport_ = nullptr;
    FrameSink         frameSink_;
    EventSink         eventSink_;

    std::thread       thread_;
    std::atomic<bool> stopFlag_ { false };
    std::atomic<bool> running_  { false };
};
