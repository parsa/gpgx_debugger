#include "BridgeServer.h"
#include "EmuHost.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#  define BAD_SOCK  INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using socket_t = int;
#  define CLOSESOCK ::close
#  define BAD_SOCK  (-1)
#endif

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const uint8_t* data, size_t n)
{
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = (i + 1 < n) ? data[i + 1] : 0;
        const unsigned b2 = (i + 2 < n) ? data[i + 2] : 0;
        const unsigned v = (b0 << 16) | (b1 << 8) | b2;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? kB64[v & 63]        : '=';
    }
    return out;
}

std::string toHex(const uint8_t* data, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { out += d[data[i] >> 4]; out += d[data[i] & 15]; }
    return out;
}

std::vector<uint8_t> fromHex(const std::string& s)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

uint32_t parseU32(const std::string& s, int base = 16)
{
    return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, base));
}

} // namespace

// ---------------------------------------------------------------------------
bool BridgeServer::start(unsigned short port)
{
    stop();
    stopFlag_.store(false);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK) return false;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    // Loopback only: this is an unauthenticated control channel with full
    // memory read/write over the emulated machine — it must never be reachable
    // from another host.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(fd, 1) != 0) {
        CLOSESOCK(fd);
        return false;
    }

    listenFd_ = static_cast<long long>(fd);
    port_     = port;
    running_.store(true);
    thread_   = std::thread(&BridgeServer::serve, this);
    return true;
}

void BridgeServer::stop()
{
    if (!thread_.joinable()) { running_.store(false); return; }
    stopFlag_.store(true);
    if (listenFd_ >= 0) {
        CLOSESOCK(static_cast<socket_t>(listenFd_));   // unblocks accept()
        listenFd_ = -1;
    }
    thread_.join();
    running_.store(false);
#ifdef _WIN32
    WSACleanup();
#endif
}

void BridgeServer::serve()
{
    while (!stopFlag_.load()) {
        socket_t client = ::accept(static_cast<socket_t>(listenFd_), nullptr, nullptr);
        if (client == BAD_SOCK) break;               // listener closed by stop()

        std::string buf;
        char chunk[4096];
        while (!stopFlag_.load()) {
            const int n = ::recv(client, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            buf.append(chunk, n);

            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                std::string reply = handle(line);
                reply += '\n';
                size_t sent = 0;
                while (sent < reply.size()) {
                    const int w = ::send(client, reply.data() + sent, (int)(reply.size() - sent), 0);
                    if (w <= 0) { sent = reply.size(); break; }
                    sent += w;
                }
            }
        }
        CLOSESOCK(client);
    }
}

// ---------------------------------------------------------------------------
std::string BridgeServer::handle(const std::string& line)
{
    if (!host_) return "err no host";
    IDebugBackend* be = host_->backend();

    std::istringstream is(line);
    std::string cmd;
    is >> cmd;
    if (cmd.empty()) return "err empty";

    auto arg = [&is]() { std::string s; is >> s; return s; };

    if (cmd == "ping")    return "ok smd_dgx";

    if (cmd == "status") {
        char out[128];
        std::snprintf(out, sizeof out, "ok running=%d paused=%d pc=%06X",
                      host_->isRunning() ? 1 : 0, be->isPaused() ? 1 : 0,
                      be->getM68kRegs().pc & 0xFFFFFF);
        return out;
    }

    if (cmd == "regs68k") {
        const M68kRegs r = be->getM68kRegs();
        std::ostringstream o;
        o << "ok";
        for (int i = 0; i < 8; ++i) o << " d" << i << "=" << std::hex << r.d[i];
        for (int i = 0; i < 8; ++i) o << " a" << i << "=" << std::hex << r.a[i];
        o << " pc=" << std::hex << r.pc << " sr=" << r.sr
          << " usp=" << r.usp << " isp=" << r.isp;
        return o.str();
    }

    if (cmd == "regsz80") {
        const Z80Regs r = be->getZ80Regs();
        std::ostringstream o;
        o << "ok" << std::hex
          << " af=" << r.af << " bc=" << r.bc << " de=" << r.de << " hl=" << r.hl
          << " ix=" << r.ix << " iy=" << r.iy << " sp=" << r.sp << " pc=" << r.pc
          << " i=" << (int)r.i << " r=" << (int)r.r << " im=" << (int)r.im
          << " halt=" << (int)r.halt;
        return o.str();
    }

    if (cmd == "vdp") {
        const VdpState v = be->getVdpState();
        std::ostringstream o;
        o << "ok reg=" << toHex(v.reg, 24)
          << " status=" << std::hex << v.status
          << " dma_len=" << v.dma_len << " dma_src=" << v.dma_src
          << " dma_type=" << (int)v.dma_type;
        return o.str();
    }

    if (cmd == "read") {
        const uint32_t a = parseU32(arg());
        const uint32_t n = parseU32(arg(), 10);
        if (n == 0 || n > (1u << 20)) return "err bad size";
        const auto d = be->readMemory(a, n);
        return "ok " + toHex(d.data(), d.size());
    }

    if (cmd == "write") {
        const uint32_t a = parseU32(arg());
        const auto d = fromHex(arg());
        if (d.empty()) return "err no data";
        return be->writeMemory(a, d.data(), (uint32_t)d.size()) ? "ok" : "err write failed";
    }

    if (cmd == "regions") {
        std::ostringstream o;
        o << "ok";
        for (const auto& r : be->getMemRegions())
            o << " " << r.id << ":" << r.name << ":" << std::hex << r.base
              << ":" << r.size << ":" << (r.writable ? 1 : 0) << std::dec;
        return o.str();
    }

    if (cmd == "readregion") {
        const int id     = (int)parseU32(arg(), 10);
        const uint32_t o = parseU32(arg());
        const uint32_t n = parseU32(arg(), 10);
        if (n == 0 || n > (1u << 20)) return "err bad size";
        const auto d = be->readRegion(id, o, n);
        return "ok " + toHex(d.data(), d.size());
    }

    if (cmd == "pause")  { be->pause();    return "ok"; }
    if (cmd == "resume") { be->resume();   return "ok"; }
    // optional trailing "z80" selects the sound CPU
    if (cmd == "stepi" || cmd == "stepo") {
        const Cpu cpu = (arg() == "z80") ? Cpu::Z80 : Cpu::M68K;
        if (cmd == "stepi") be->stepInto(cpu); else be->stepOver(cpu);
        return "ok";
    }

    if (cmd == "pad") {
        be->setPad(0, (uint16_t)parseU32(arg()));
        return "ok";
    }

    if (cmd == "bpadd") {
        Breakpoint bp;
        const std::string t = arg();
        bp.type  = (t == "r") ? BpType::Read : (t == "w") ? BpType::Write : BpType::PC;
        bp.start = parseU32(arg());
        bp.end   = parseU32(arg());
        if (bp.end < bp.start) bp.end = bp.start;
        return "ok " + std::to_string(be->addBreakpoint(bp));
    }

    if (cmd == "bpdel")  { be->removeBreakpoint((int)parseU32(arg(), 10)); return "ok"; }
    if (cmd == "bpclear"){ be->clearBreakpoints(); return "ok"; }

    if (cmd == "bplist") {
        std::ostringstream o;
        o << "ok";
        for (const auto& b : be->getBreakpoints())
            o << " " << b.id << ","
              << (b.type == BpType::PC ? "x" : b.type == BpType::Read ? "r" : "w") << ","
              << std::hex << b.start << "," << b.end << std::dec;
        return o.str();
    }

    // Whole-machine operations: must not race the core.
    if (cmd == "savestate" || cmd == "loadstate") {
        std::string path;
        std::getline(is, path);
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(0, 1);
        if (path.empty()) return "err no path";
        const bool save = (cmd == "savestate");
        bool okFlag = false;
        if (!host_->invoke([&] { okFlag = save ? be->saveState(path.c_str())
                                               : be->loadState(path.c_str()); }))
            return "err emulator not running";
        return okFlag ? "ok" : "err state io failed";
    }

    if (cmd == "frame") {
        // Visible viewport as raw RGB565, base64. The Python side turns it
        // into a PNG so this stays dependency-free.
        const VdpState v = be->getVdpState();
        (void)v;
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;
        if (!host_->invoke([&] { host_->copyViewport(rgb, w, h); }))
            return "err emulator not running";
        if (rgb.empty()) return "err no frame";
        std::ostringstream o;
        o << "ok " << w << " " << h << " " << base64(rgb.data(), rgb.size());
        return o.str();
    }

    return "err unknown command";
}
