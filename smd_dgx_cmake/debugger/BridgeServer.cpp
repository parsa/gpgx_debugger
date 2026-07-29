#include "BridgeServer.h"
#include "EmuHost.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <memory>
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

    if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(fd, 4) != 0) {
        CLOSESOCK(fd);
        return false;
    }

    listenFd_ = static_cast<long long>(fd);
    port_     = port;
    running_.store(true);
    host_->addEventSink([this](const DebugEvent& ev) { onEmuEvent(&ev); });
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
    // Close the client sockets so their recv() returns and the threads exit;
    // otherwise a connected debugger keeps this object alive past the host.
    {
        std::lock_guard<std::mutex> lk(clientsMx_);
        for (auto& c : clients_)
            if (c->fd >= 0) CLOSESOCK(static_cast<socket_t>(c->fd));
    }
    thread_.join();
    for (auto& t : clientThreads_) if (t.joinable()) t.join();
    clientThreads_.clear();
    { std::lock_guard<std::mutex> lk(clientsMx_); clients_.clear(); }
    running_.store(false);
#ifdef _WIN32
    WSACleanup();
#endif
}

void BridgeServer::serve()
{
    while (!stopFlag_.load()) {
        socket_t fd = ::accept(static_cast<socket_t>(listenFd_), nullptr, nullptr);
        if (fd == BAD_SOCK) break;               // listener closed by stop()

        auto c = std::make_shared<Client>();
        c->fd = static_cast<long long>(fd);
        {
            std::lock_guard<std::mutex> lk(clientsMx_);
            clients_.push_back(c);
            clientThreads_.emplace_back(&BridgeServer::serveClient, this, c);
        }
    }
}

void BridgeServer::serveClient(std::shared_ptr<Client> c)
{
    const socket_t fd = static_cast<socket_t>(c->fd);
    std::string buf;
    char chunk[4096];

    while (!stopFlag_.load()) {
        const int n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, n);

        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::string reply = handle(line, *c);
            reply += '\n';
            size_t sent = 0;
            while (sent < reply.size()) {
                const int w = ::send(fd, reply.data() + sent, (int)(reply.size() - sent), 0);
                if (w <= 0) { sent = reply.size(); break; }
                sent += w;
            }
        }
    }

    CLOSESOCK(fd);
    std::lock_guard<std::mutex> lk(clientsMx_);
    for (size_t i = 0; i < clients_.size(); ++i)
        if (clients_[i] == c) { clients_.erase(clients_.begin() + i); break; }
}

// Runs on the EMULATION thread. Queue and return — never touch the backend
// from here, and never block: the emulator is waiting on us.
void BridgeServer::onEmuEvent(const void* evp)
{
    const DebugEvent& ev = *static_cast<const DebugEvent*>(evp);
    std::lock_guard<std::mutex> lk(clientsMx_);
    const uint64_t seq = nextSeq_++;
    for (auto& c : clients_) {
        if (c->events.size() >= 256) { c->events.pop_front(); ++c->dropped; }
        c->events.push_back(QueuedEvent{ seq, uint8_t(ev.type), uint8_t(ev.cpu), ev.pc });
    }
}

std::string BridgeServer::handleEvents(Client& c, int max)
{
    std::lock_guard<std::mutex> lk(clientsMx_);
    std::ostringstream o;
    std::vector<std::string> tuples;

    if (c.dropped) {                       // tell the client it missed some
        std::ostringstream d;
        d << "0,dropped," << std::dec << c.dropped << ",-";
        tuples.push_back(d.str());
        c.dropped = 0;
    }
    static const char* const kNames[] = { "started", "paused", "resumed", "stopped" };
    while (!c.events.empty() && (int)tuples.size() < max) {
        const QueuedEvent e = c.events.front();
        c.events.pop_front();
        std::ostringstream t;
        t << std::dec << e.seq << ","
          << (e.type < 4 ? kNames[e.type] : "?") << ","
          << std::hex << e.pc << ","
          << (e.cpu ? "z80" : "m68k");
        tuples.push_back(t.str());
    }

    o << "ok " << std::dec << tuples.size();
    for (const auto& t : tuples) o << " " << t;
    return o.str();
}

// ---------------------------------------------------------------------------
std::string BridgeServer::handle(const std::string& line, Client& client)
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
          << " af2=" << r.af2 << " bc2=" << r.bc2 << " de2=" << r.de2 << " hl2=" << r.hl2
          << " ix=" << r.ix << " iy=" << r.iy << " sp=" << r.sp << " pc=" << r.pc
          << " i=" << (int)r.i << " r=" << (int)r.r << " im=" << (int)r.im
          << " iff1=" << (int)r.iff1 << " iff2=" << (int)r.iff2
          << " halt=" << (int)r.halt << " bank=" << r.bank;
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
        for (const auto& r : be->getMemRegions()) {
            // Records are space-separated, so a name may not contain a space:
            // "RAM 68K" would split into two bogus records and every client
            // that checks the field count would silently drop the region.
            std::string name = r.name;
            for (char& c : name) if (c == ' ') c = '_';
            o << " " << r.id << ":" << name << ":" << std::hex << r.base
              << ":" << r.size << ":" << (r.writable ? 1 : 0) << std::dec;
        }
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


    if (cmd == "events") {
        int max = (int)parseU32(arg(), 10);
        if (max <= 0 || max > 256) max = 64;
        return handleEvents(client, max);
    }

    // --- register writes -------------------------------------------------
    // One command per CPU rather than per register: the get/modify/set has to
    // be atomic, and a half-applied register set is worse than none.
    if (cmd == "wreg68k") {
        M68kRegs r = be->getM68kRegs();
        for (std::string kv; is >> kv; ) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const uint32_t    v = parseU32(kv.substr(eq + 1));
            if (k.size() == 2 && (k[0] == 'd' || k[0] == 'a') && k[1] >= '0' && k[1] <= '7') {
                (k[0] == 'd' ? r.d : r.a)[k[1] - '0'] = v;
            }
            else if (k == "pc")  r.pc  = v;
            else if (k == "sr")  r.sr  = v;
            else if (k == "usp") r.usp = v;
            else if (k == "isp") r.isp = v;
        }
        be->setM68kRegs(r);
        return "ok";
    }

    if (cmd == "wregz80") {
        Z80Regs r = be->getZ80Regs();
        for (std::string kv; is >> kv; ) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const uint32_t    v = parseU32(kv.substr(eq + 1));
            if      (k == "af")  r.af  = uint16_t(v); else if (k == "bc")  r.bc  = uint16_t(v);
            else if (k == "de")  r.de  = uint16_t(v); else if (k == "hl")  r.hl  = uint16_t(v);
            else if (k == "af2") r.af2 = uint16_t(v); else if (k == "bc2") r.bc2 = uint16_t(v);
            else if (k == "de2") r.de2 = uint16_t(v); else if (k == "hl2") r.hl2 = uint16_t(v);
            else if (k == "ix")  r.ix  = uint16_t(v); else if (k == "iy")  r.iy  = uint16_t(v);
            else if (k == "sp")  r.sp  = uint16_t(v); else if (k == "pc")  r.pc  = uint16_t(v);
            else if (k == "i")   r.i   = uint8_t(v);  else if (k == "r")   r.r   = uint8_t(v);
            else if (k == "im")  r.im  = uint8_t(v);  else if (k == "halt") r.halt = uint8_t(v);
            else if (k == "iff1") r.iff1 = uint8_t(v); else if (k == "iff2") r.iff2 = uint8_t(v);
        }
        // Writing PC mid-instruction is only sound while the core is parked.
        bool done = false;
        if (!host_->invoke([&] { be->setZ80Regs(r); done = true; })) return "err emulator not running";
        return done ? "ok" : "err failed";
    }

    if (cmd == "readz80") {
        const uint32_t a = parseU32(arg());
        const uint32_t n = parseU32(arg(), 10);
        if (n == 0 || n > (1u << 16)) return "err bad size";
        const auto d = be->readZ80Memory(uint16_t(a), uint16_t(n));
        return "ok " + toHex(d.data(), d.size());
    }

    if (cmd == "writeregion") {
        const int id     = (int)parseU32(arg(), 10);
        const uint32_t o = parseU32(arg());
        const auto d = fromHex(arg());
        if (d.empty()) return "err no data";
        return be->writeRegion(id, o, d.data(), (uint32_t)d.size()) ? "ok" : "err write failed";
    }

    if (cmd == "setvdpreg") {
        const int idx = (int)parseU32(arg(), 10);
        be->setVdpReg(idx, uint8_t(parseU32(arg())));
        return "ok";
    }

    if (cmd == "callstack") {
        const Cpu cpu = (arg() == "z80") ? Cpu::Z80 : Cpu::M68K;
        std::ostringstream o;
        o << "ok";
        for (uint32_t ea : be->getCallstack(cpu)) o << " " << std::hex << ea;
        return o.str();
    }

    if (cmd == "sound") {
        const SoundState st = be->getSoundState();
        std::ostringstream o;
        o << "ok fm0=" << toHex(st.fm[0], 256) << " fm1=" << toHex(st.fm[1], 256) << " psg=";
        for (int i = 0; i < 8; ++i) o << (i ? "," : "") << std::dec << st.psg[i];
        return o.str();
    }

    if (cmd == "getpad") {
        const int port = (int)parseU32(arg(), 10);
        std::ostringstream o; o << "ok " << std::hex << be->getPad(port);
        return o.str();
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
        const uint16_t mask = (uint16_t)parseU32(arg());
        const std::string p = arg();
        be->setPad(p.empty() ? 0 : (int)parseU32(p, 10), mask);
        return "ok";
    }

    if (cmd == "bpadd") {
        Breakpoint bp;
        const std::string t = arg();
        bp.type  = (t == "r") ? BpType::Read : (t == "w") ? BpType::Write : BpType::PC;
        bp.start = parseU32(arg());
        bp.end   = parseU32(arg());
        if (bp.end < bp.start) bp.end = bp.start;
        // Optional key=value tail. cpu and vdp are not cosmetic: matchBreakpoint
        // requires both to agree or the breakpoint silently never fires.
        for (std::string kv; is >> kv; ) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if      (k == "cpu")   bp.cpu     = (v == "z80") ? Cpu::Z80 : Cpu::M68K;
            else if (k == "vdp")   bp.is_vdp  = (v != "0");
            else if (k == "elang") bp.elang   = parseU32(v, 10);
            else if (k == "cond") {           // last: conditions contain spaces
                std::string rest;
                std::getline(is, rest);
                bp.condition = v + rest;
                break;
            }
        }
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
              << std::hex << b.start << "," << b.end << std::dec
              << "," << (b.cpu == Cpu::Z80 ? "z80" : "m68k")
              << "," << (b.is_vdp ? 1 : 0) << "," << (b.enabled ? 1 : 0);
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
