#pragma once
#include "GpgxBackend.h"
#include "DebugApi.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    // (RGB565, gpgx t_bitmap geometry) plus the active viewport rect — the
    // visible area varies with H32/H40 and border settings. Omit for a truly
    // headless run.
    using FrameSink = std::function<void(const uint8_t* data, int width, int height, int pitch,
                                         int vpX, int vpY, int vpW, int vpH)>;
    // Receives one frame's worth of 16-bit stereo samples. Omit for silence.
    using AudioSink = std::function<void(const int16_t* stereo, int frames)>;
    using EventSink = std::function<void(const DebugEvent&)>;

    EmuHost();
    ~EmuHost();

    // Attach a transport (e.g. InProcTransport). The host will drain commands
    // from it (between frames and during pauses) and post events to it. May be
    // null for a pure direct-access host.
    void setTransport(IDebugTransport* t) { transport_ = t; }
    void setFrameSink(FrameSink s) { frameSink_ = std::move(s); }
    void setAudioSink(AudioSink s) { audioSink_ = std::move(s); }
    // Several consumers now: the IDA plugin turns events into debug_event_t,
    // the bridge queues them for attached clients. Sinks run on the EMULATION
    // thread — keep them short and never call back into run control.
    void addEventSink(EventSink s);

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

    // Run fn on the emulation thread and wait for it. Use for anything that
    // must not race the core — save states above all. Safe to call while the
    // emulator is paused: the pause pump drains these too. Returns false if
    // the emulator is not running (fn is then not executed).
    bool invoke(const std::function<void()>& fn);

    // Copy the visible viewport as tightly packed RGB565 (w*h*2 bytes). Call
    // from the emulation thread — i.e. through invoke() — so the framebuffer
    // is not being redrawn underneath.
    void copyViewport(std::vector<uint8_t>& out, int& w, int& h) const;

private:
    void run(std::string romPath);
    void drainCommands();
    void drainTasks();
    void emitEvent(const DebugEvent& ev);   // not `emit`: clashes with Qt's macro

    GpgxBackend       backend_;
    IDebugTransport*  transport_ = nullptr;
    FrameSink         frameSink_;
    AudioSink         audioSink_;
    std::mutex             sinkMx_;
    std::vector<EventSink> eventSinks_;

    std::thread       thread_;
    std::atomic<bool> stopFlag_ { false };
    std::atomic<bool> running_  { false };

    // Work handed to the emulation thread by invoke().
    struct Task { const std::function<void()>* fn; bool done; };
    std::mutex              taskMx_;
    std::condition_variable taskCv_;
    std::deque<Task*>       tasks_;
};
