#pragma once
#include <atomic>
#include <string>
#include <thread>

class EmuHost;

// Localhost control socket over an EmuHost. Exists so an external agent (the
// Python MCP server) can drive the *same* emulator session the user is looking
// at, rather than a private copy.
//
// The wire format is deliberately dumb — one request per line, one response
// per line, "ok ..." or "err ..." — so this side needs no JSON library and no
// dependencies beyond sockets. All structure lives in the Python server.
//
// Threading: the server runs on its own thread and serves one client at a
// time. Reads go straight to the backend (racy-but-benign while running,
// exact while paused, same contract as the debug views); anything that must
// not race the core is routed through EmuHost::invoke().
class BridgeServer {
public:
    explicit BridgeServer(EmuHost* host) : host_(host) {}
    ~BridgeServer() { stop(); }

    bool start(unsigned short port = 27042);
    void stop();
    bool isRunning() const { return running_.load(); }
    unsigned short port() const { return port_; }

private:
    void serve();
    std::string handle(const std::string& line);

    EmuHost*          host_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> stopFlag_{ false };
    unsigned short    port_ = 0;
    long long         listenFd_ = -1;
};
