// BridgeServer <-> RemoteBackend over a real socket.
//
// This is the seam where the two clients (the Python MCP server and the Z80
// IDA database) meet the emulator, and it is a hand-rolled text protocol: every
// field is an opportunity for the two sides to disagree about a format. The
// "regions" bug — a name containing the record separator, so every client
// silently dropped five of nine regions — was exactly that, and went unnoticed
// because nothing ever compared the two sides.

#include "fixture.h"

#include "debugger/BridgeServer.h"
#include "debugger/HostRunSync.h"
#include "debugger/RemoteBackend.h"
#include "debugger/SocketCompat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace synth;

namespace {

// Each test gets its own port: the suite may run alongside a live emulator on
// the default one, and a test that quietly attaches to the user's session
// instead of its own would be worse than a failure.
unsigned short portFor(int n) { return (unsigned short)(28100 + n); }

struct Wired {
    t::Emu       emu;
    BridgeServer bridge{ nullptr };
    RemoteBackend remote;
    bool ok = false;

    explicit Wired(int slot)
    {
        if (!emu.start()) return;
        if (!emu.pauseAndWait()) return;
        bridge.~BridgeServer();
        new (&bridge) BridgeServer(emu.host());
        if (!bridge.start(portFor(slot))) return;
        if (!remote.connect("127.0.0.1", portFor(slot))) return;
        ok = true;
    }
};

} // namespace

// REGRESSION: region names contain spaces, and records were space-separated.
TEST(bridge_regions_survive_names_with_spaces)
{
    Wired w(1);
    REQUIRE(w.ok);

    const auto direct = w.emu.backend()->getMemRegions();
    const auto wire   = w.remote.getMemRegions();

    REQUIRE(wire.size() == direct.size());
    for (size_t i = 0; i < direct.size(); ++i) {
        CHECK_EQ(wire[i].id,   direct[i].id);
        CHECK_EQ(wire[i].size, direct[i].size);
        CHECK_EQ(wire[i].base, direct[i].base);
        // The name may be transported with spaces escaped, but it must still
        // identify the region — never truncate at the separator.
        CHECK(wire[i].name.size() >= direct[i].name.size());
    }
    // The regions whose names contain a space are the ones that vanished.
    const bool has68k = std::any_of(wire.begin(), wire.end(),
        [](const MemRegion& m) { return m.id == 1 && m.size == 0x10000; });
    const bool hasZ80 = std::any_of(wire.begin(), wire.end(),
        [](const MemRegion& m) { return m.id == 2 && m.size == 0x2000; });
    CHECK(has68k);
    CHECK(hasZ80);
}

TEST(bridge_memory_matches_the_backend_byte_for_byte)
{
    Wired w(2);
    REQUIRE(w.ok);

    const auto direct = w.emu.backend()->readMemory(kMarkerAddr, 18);
    const auto wire   = w.remote.readMemory(kMarkerAddr, 18);
    CHECK(direct == wire);

    const auto dz = w.emu.backend()->readZ80Memory(0, 32);
    const auto wz = w.remote.readZ80Memory(0, 32);
    CHECK(dz == wz);

    const auto dr = w.emu.backend()->readRegion(0, 0x100, 16);
    const auto wr = w.remote.readRegion(0, 0x100, 16);
    CHECK(dr == wr);
}

TEST(bridge_writes_reach_the_emulator)
{
    Wired w(3);
    REQUIRE(w.ok);

    const uint8_t pat[4] = { 0x5A, 0xA5, 0x0F, 0xF0 };
    w.remote.writeMemory(0xFF4000, pat, 4);
    const auto back = w.emu.backend()->readMemory(0xFF4000, 4);
    REQUIRE(back.size() == 4);
    CHECK(std::equal(pat, pat + 4, back.begin()));
}

// Every register the Z80 debugger_t shows must survive the wire. A field the
// server forgets to emit reads as zero on the client and looks like real state.
TEST(bridge_z80_registers_are_complete)
{
    Wired w(4);
    REQUIRE(w.ok);

    Z80Regs want{};
    want.af = 0x1234; want.bc = 0x2345; want.de = 0x3456; want.hl = 0x4567;
    want.af2 = 0x5678; want.bc2 = 0x6789; want.de2 = 0x789A; want.hl2 = 0x89AB;
    want.ix = 0x9ABC; want.iy = 0xABCD; want.sp = 0x1FF0; want.pc = 0x0100;
    want.i = 0x12; want.r = 0x34; want.im = 1; want.iff1 = 1; want.iff2 = 0;
    w.emu.backend()->setZ80Regs(want);

    const Z80Regs got = w.remote.getZ80Regs();
    CHECK_EQ(got.af, want.af);   CHECK_EQ(got.bc, want.bc);
    CHECK_EQ(got.de, want.de);   CHECK_EQ(got.hl, want.hl);
    CHECK_EQ(got.af2, want.af2); CHECK_EQ(got.bc2, want.bc2);
    CHECK_EQ(got.de2, want.de2); CHECK_EQ(got.hl2, want.hl2);
    CHECK_EQ(got.ix, want.ix);   CHECK_EQ(got.iy, want.iy);
    CHECK_EQ(got.sp, want.sp);   CHECK_EQ(got.pc, want.pc);
    CHECK_EQ(got.i, want.i);     CHECK_EQ(got.r, want.r);
    CHECK_EQ(got.im, want.im);   CHECK_EQ(got.iff1, want.iff1);
    CHECK_EQ(got.bank, w.emu.backend()->getZ80Regs().bank);
}

TEST(bridge_m68k_registers_round_trip)
{
    Wired w(5);
    REQUIRE(w.ok);

    M68kRegs want = w.emu.backend()->getM68kRegs();
    for (int i = 0; i < 8; ++i) { want.d[i] = 0x11111111u * (i + 1); }
    want.a[0] = 0x00FF1000; want.a[7] = kInitSp;
    w.remote.setM68kRegs(want);

    const M68kRegs got = w.emu.backend()->getM68kRegs();
    for (int i = 0; i < 8; ++i) CHECK_EQ(got.d[i], want.d[i]);
    CHECK_EQ(got.a[0], want.a[0]);

    const M68kRegs viaWire = w.remote.getM68kRegs();
    for (int i = 0; i < 8; ++i) CHECK_EQ(viaWire.d[i], want.d[i]);
}

TEST(bridge_breakpoints_round_trip_with_their_cpu)
{
    Wired w(6);
    REQUIRE(w.ok);

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = 0x0123; bp.end = 0x0123;
    const int id = w.remote.addBreakpoint(bp);
    CHECK(id >= 0);

    const auto listed = w.remote.getBreakpoints();
    REQUIRE(listed.size() == 1);
    CHECK_EQ(listed[0].start, 0x0123u);
    CHECK(listed[0].cpu == Cpu::Z80);        // losing this aliases the two CPUs
    CHECK(listed[0].type == BpType::PC);

    // ...and the server must agree.
    const auto server = w.emu.backend()->getBreakpoints();
    REQUIRE(server.size() == 1);
    CHECK(server[0].cpu == Cpu::Z80);

    w.remote.removeBreakpoint(listed[0].id);
    CHECK(w.remote.getBreakpoints().empty());
}

TEST(bridge_events_report_the_stopping_cpu)
{
    Wired w(7);
    REQUIRE(w.ok);

    // Drain whatever the attach itself produced.
    RemoteBackend::RemoteEvent ev;
    while (w.remote.pollEvent(ev)) {}

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;
    w.remote.addBreakpoint(bp);
    w.remote.resume();

    bool sawPause = false;
    for (int i = 0; i < 60 && !sawPause; ++i) {
        while (w.remote.pollEvent(ev))
            if (ev.type == DebugEvent::Type::Paused) {
                sawPause = true;
                CHECK_EQ(ev.pc, kSubEntry);
                CHECK(ev.cpu == Cpu::M68K);
                break;
            }
        if (!sawPause) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(sawPause);
    w.remote.clearBreakpoints();
}

TEST(bridge_serves_two_clients_independently)
{
    Wired w(8);
    REQUIRE(w.ok);

    RemoteBackend second;
    REQUIRE(second.connect("127.0.0.1", portFor(8)));

    // Both see the same machine...
    const auto a = w.remote.readMemory(0x100, 16);
    const auto b = second.readMemory(0x100, 16);
    CHECK(a == b);

    // ...but each has its own event queue, so one draining does not starve the
    // other. Drain both, then make one event and check both receive it.
    RemoteBackend::RemoteEvent ev;
    while (w.remote.pollEvent(ev)) {}
    while (second.pollEvent(ev)) {}

    w.remote.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    w.remote.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int seenA = 0, seenB = 0;
    while (w.remote.pollEvent(ev)) ++seenA;
    while (second.pollEvent(ev))  ++seenB;
    CHECK(seenA > 0);
    CHECK(seenB > 0);
}

// ---------------------------------------------------------------------------
// The agent loop: set a breakpoint, resume, wait, look, advance frames.
//
// These are the primitives that make the bridge usable without a human in the
// loop. Polling `events` can substitute for `wait` only by burning CPU, and
// nothing at all could substitute for frame-accurate advance: while the machine
// is being debugged it is not bound to the wall clock, so an agent cannot time
// an input by sleeping.
// ---------------------------------------------------------------------------

// Raw access, because these commands have no IDebugBackend counterpart.
namespace {

struct Line {
    RemoteBackend* rb;
    // The protocol is one line in, one line out; RemoteBackend::call is private,
    // so the tests speak it directly over their own socket.
};

class Raw {
public:
    bool connect(unsigned short port)
    {
        sockcompat::netInit();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == BAD_SOCK) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        return ::connect(fd_, (sockaddr*)&a, sizeof a) == 0;
    }
    ~Raw() { if (fd_ != BAD_SOCK) CLOSESOCK(fd_); }

    std::string cmd(const std::string& c)
    {
        std::string req = c + "\n";
        if (!sockcompat::sendAll(fd_, req.data(), req.size())) return {};
        char buf[65536];
        size_t nl;
        while ((nl = rx_.find('\n')) == std::string::npos) {
            const int n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) return {};
            rx_.append(buf, n);
        }
        std::string r = rx_.substr(0, nl);
        rx_.erase(0, nl + 1);
        return r;
    }

private:
    socket_t fd_ = BAD_SOCK;
    std::string rx_;
};

} // namespace

TEST(bridge_wait_blocks_until_a_breakpoint_and_names_it)
{
    Wired w(10);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(10)));
    // connect() can return before accept() registers this client for events.
    // A reply proves registration before the breakpoint can fire.
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;
    const int id = w.emu.backend()->addBreakpoint(bp);
    REQUIRE(id > 0);

    w.emu.backend()->resume();
    const std::string r = raw.cmd("wait 4000");
    // ok paused pc=... cpu=m68k bp=<id> seq=...
    CHECK(r.rfind("ok paused", 0) == 0);
    char want[32];
    std::snprintf(want, sizeof want, "bp=%d", id);
    CHECK(r.find(want) != std::string::npos);      // WHICH breakpoint, not just that one hit
    CHECK(r.find("cpu=m68k") != std::string::npos);

    w.emu.backend()->clearBreakpoints();
}

TEST(bridge_wait_reports_timeout_rather_than_hanging)
{
    Wired w(11);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(11)));

    w.emu.backend()->resume();                     // nothing armed: nothing to stop on
    CHECK_STR(raw.cmd("wait 400"), "ok timeout");
}

TEST(bridge_frame_advance_runs_exactly_that_many_frames)
{
    Wired w(12);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(12)));

    CHECK_STR(raw.cmd("frameadv 3"), "ok");
    const std::string r = raw.cmd("wait 4000");
    CHECK(r.rfind("ok paused", 0) == 0);
    CHECK(r.find("bp=-1") != std::string::npos);   // stopped by the advance, not a breakpoint
    CHECK(w.emu.backend()->isPaused());
}

TEST(bridge_search_finds_a_pattern_in_a_region)
{
    Wired w(13);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(13)));

    // Our own marker, at a known ROM offset.
    const std::string pat = "534D442D444758";           // "SMD-DGX"
    const std::string r = raw.cmd("search 0 " + pat + " 16");
    REQUIRE(r.rfind("ok", 0) == 0);

    char want[32];
    std::snprintf(want, sizeof want, "%x", kMarkerAddr);
    CHECK(r.find(want) != std::string::npos);

    // A pattern that is not there returns success with no hits, not an error.
    CHECK_STR(raw.cmd("search 0 deadbeefcafe 16"), "ok");
}

TEST(bridge_diff_reports_what_changed_since_the_snapshot)
{
    Wired w(14);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(14)));

    // Region 1 is 68000 work RAM.
    REQUIRE(raw.cmd("snap 1").rfind("ok", 0) == 0);
    CHECK_STR(raw.cmd("diff 1 8"), "ok");          // nothing has moved yet

    const uint8_t v[2] = { 0xA5, 0x5A };
    w.emu.backend()->writeRegion(1, 0x0500, v, 2);

    const std::string r = raw.cmd("diff 1 8");
    REQUIRE(r.rfind("ok ", 0) == 0);
    CHECK(r.find("500:") != std::string::npos);    // offset:old:new
    CHECK(r.find(":a5") != std::string::npos);
    CHECK(r.find("501:") != std::string::npos);

    // A region never snapped is an error, not a silent empty result.
    CHECK(raw.cmd("diff 3 8").rfind("err", 0) == 0);
}

// ---------------------------------------------------------------------------
// Two emulators at once.
//
// A fixed port made a second game silently unreachable, and left every client
// unable to say whose game it had reached. The server now takes the first free
// port in its range and says who it is; the client scans and picks.
// ---------------------------------------------------------------------------
TEST(bridge_two_sessions_get_different_ports_and_are_told_apart)
{
    // Two *servers*, one emulator. Two emulators cannot be tested in one
    // process at all: the gpgx core is global C state and GpgxBackend claims a
    // single global CPU hook, so a second instance silently disables the first.
    // Two games therefore means two processes — which is exactly the case this
    // port allocation exists to serve.
    t::Emu a;
    REQUIRE(a.start());
    REQUIRE(a.pauseAndWait());

    BridgeServer sa(a.host()), sb(a.host());
    REQUIRE(sa.start());                       // 0 = pick a free one
    REQUIRE(sb.start());
    CHECK(sa.port() != 0);
    CHECK(sb.port() != 0);
    CHECK(sa.port() != sb.port());             // the whole point

    // Both are discoverable, and each reports the port it actually holds.
    const auto found = RemoteBackend::discover();
    const bool sawA = std::any_of(found.begin(), found.end(),
        [&](const SessionInfo& s) { return s.port == sa.port(); });
    const bool sawB = std::any_of(found.begin(), found.end(),
        [&](const SessionInfo& s) { return s.port == sb.port(); });
    CHECK(sawA);
    CHECK(sawB);

    // Connecting to a named port reaches that one, and says so.
    RemoteBackend rb;
    REQUIRE(rb.connect("127.0.0.1", sb.port()));
    CHECK_EQ(rb.session().port, sb.port());
    CHECK(rb.session().pid != 0);
}

TEST(bridge_session_identifies_the_game)
{
    Wired w(15);
    REQUIRE(w.ok);

    const SessionInfo& s = w.remote.session();
    CHECK(s.pid != 0);
    CHECK_EQ(s.port, portFor(15));
    // The synthetic ROM has no product code or title, so those may be "-";
    // the calculated checksum is what actually distinguishes two games.
    const SessionInfo direct = w.emu.host()->gpgx()->sessionInfo();
    CHECK_EQ(s.crc, direct.crc);
}

TEST(bridge_client_stops_claiming_health_after_the_server_goes)
{
    // A dead socket that still reports connected turns every later read into
    // zeros that look like real emulator state.
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    auto* srv = new BridgeServer(e.host());
    REQUIRE(srv->start(portFor(16)));

    RemoteBackend rb;
    REQUIRE(rb.connect("127.0.0.1", portFor(16)));
    CHECK(rb.isConnected());

    delete srv;                                 // server goes away
    rb.getM68kRegs();                           // first call notices the close
    CHECK(!rb.isConnected());
}

// ---------------------------------------------------------------------------
// A host that models run control and breakpoints itself (IDA does) must be the
// one that performs them, or its view goes stale: IDA owns the Breakpoints
// window and the run state, and there is no debug event meaning "resumed", so
// a bridge client reaching straight into the backend leaves IDA convinced the
// machine is still stopped at a breakpoint it has already cleared.
// ---------------------------------------------------------------------------
TEST(bridge_routes_mutations_through_the_host_when_one_is_installed)
{
    Wired w(17);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(17)));

    struct Seen {
        int resume = 0, add = 0, del = 0, clear = 0;
    } seen;

    EmuHost::HostMutations hm;
    // A host that claims an operation must actually perform it — IDA reaches
    // the backend through ev_resume; here we stand in for that.
    hm.resume = [&] { ++seen.resume; w.emu.backend()->resume(); return true; };
    hm.addBreakpoint = [&](const Breakpoint&, int* id) {
        ++seen.add; if (id) *id = 4242; return true;
    };
    hm.removeBreakpoint = [&](int) { ++seen.del;   return true; };
    hm.clearBreakpoints = [&]      { ++seen.clear; return true; };
    w.emu.host()->setHostMutations(hm);

    CHECK_STR(raw.cmd("resume"), "ok");
    CHECK_STR(raw.cmd("pause"),  "ok");
    // The id the host reports is the one the client is told.
    CHECK_STR(raw.cmd("bpadd x 200 200"), "ok 4242");
    CHECK_STR(raw.cmd("bpdel 4242"), "ok");
    CHECK_STR(raw.cmd("bpclear"), "ok");

    CHECK_EQ(seen.resume, 1);
    // A pause is not the host's to grant: it reaches the emulator directly.
    CHECK(raw.cmd("wait 2000").rfind("ok paused", 0) == 0);
    CHECK_EQ(seen.add,    1);
    CHECK_EQ(seen.del,    1);
    CHECK_EQ(seen.clear,  1);

    // ...and the backend was NOT touched behind the host's back: the host said
    // it handled the add, so no breakpoint should have been created here.
    CHECK(w.emu.backend()->getBreakpoints().empty());

    w.emu.host()->setHostMutations({});
}

TEST(bridge_reports_a_breakpoint_the_host_could_not_delete)
{
    // IDA may refuse to delete its breakpoint; the backend copy then stays,
    // and the client must not be told "ok".
    Wired w(22);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(22)));

    Breakpoint bp;
    bp.type = BpType::PC; bp.start = kSubEntry; bp.end = kSubEntry;
    const int id = w.emu.backend()->addBreakpoint(bp);
    REQUIRE(id > 0);

    EmuHost::HostMutations hm;
    hm.removeBreakpoint = [](int) { return true; };   // handled, but kept
    w.emu.host()->setHostMutations(hm);
    CHECK_STR(raw.cmd("bpdel " + std::to_string(id)), "err not deleted");
    CHECK_EQ(w.emu.backend()->getBreakpoints().size(), size_t(1));

    hm.removeBreakpoint = [&](int i) { w.emu.backend()->removeBreakpoint(i); return true; };
    w.emu.host()->setHostMutations(hm);
    CHECK_STR(raw.cmd("bpdel " + std::to_string(id)), "ok");
    CHECK(w.emu.backend()->getBreakpoints().empty());
    w.emu.host()->setHostMutations({});
}

TEST(bridge_pause_reaches_a_halted_68000_and_says_why)
{
    Wired w(23);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(23)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    w.emu.parkM68kAt(kHaltEntry);
    w.emu.backend()->resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_STR(raw.cmd("pause"), "ok");
    const std::string r = raw.cmd("wait 2000");
    CHECK(r.rfind("ok paused", 0) == 0);
    CHECK(r.find(" reason=halted") != std::string::npos);
    const std::string s = raw.cmd("status");
    CHECK(s.find("paused=1") != std::string::npos);
    CHECK(s.find(" reason=halted") != std::string::npos);
}

TEST(bridge_reports_and_catches_exceptions)
{
    Wired w(24);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(24)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    CHECK_STR(raw.cmd("catch"), "ok mask=0");
    CHECK_STR(raw.cmd("catch faults"), "ok mask=cffc");
    CHECK(raw.cmd("catch zz").rfind("err usage", 0) == 0);

    M68kRegs r = w.emu.backend()->getM68kRegs();
    r.pc = kIllegalEntry; r.sr = 0x2700; r.a[7] = kInitSp;
    w.emu.backend()->setM68kRegs(r);
    CHECK_STR(raw.cmd("resume"), "ok");
    const std::string s = raw.cmd("wait 2000");
    CHECK(s.rfind("ok paused", 0) == 0);
    CHECK(s.find(" reason=exception vector=4 name=illegal fault_pc=000520") != std::string::npos);

    // Not caught: no stop, but the event stream still names it.
    CHECK_STR(raw.cmd("catch none"), "ok mask=0");
    r = w.emu.backend()->getM68kRegs();
    r.pc = kIllegalEntry; r.sr = 0x2700; r.a[7] = kInitSp;
    w.emu.backend()->setM68kRegs(r);
    raw.cmd("events 256");                                  // drain
    CHECK_STR(raw.cmd("resume"), "ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string ev = raw.cmd("events 16");
    CHECK(ev.find(",exception,520,m68k,-1,vector=4;name=illegal;") != std::string::npos);
    CHECK_STR(raw.cmd("pause"), "ok");
}

// Each stop kind, as an agent sees it: the old leading fields unchanged, the
// reason (and what it carries) appended.
TEST(bridge_wait_names_every_stop_reason)
{
    Wired w(25);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(25)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    auto waitStop = [&](const char* expectTail) {
        const std::string r = raw.cmd("wait 3000");
        CHECK(r.rfind("ok paused pc=", 0) == 0);
        CHECK(r.find(" cpu=m68k bp=") != std::string::npos);
        CHECK(r.find(" seq=") != std::string::npos);
        if (r.find(expectTail) == std::string::npos) CHECK_STR(r, expectTail);
        return r;
    };

    w.emu.parkM68kAt(kJsr);
    CHECK(raw.cmd("stepi").rfind("ok", 0) == 0);
    waitStop(" reason=step");

    const std::string add = raw.cmd("bpadd x 300 300");
    REQUIRE(add.rfind("ok ", 0) == 0);
    const std::string id = add.substr(3);
    w.emu.parkM68kAt(kJsr);
    CHECK_STR(raw.cmd("resume"), "ok");
    waitStop(("bp=" + id + " seq=").c_str());
    CHECK_STR(raw.cmd("bpclear"), "ok");

    raw.cmd("bpadd w ff4276 ff4279");
    w.emu.parkM68kAt(kDataEntry);
    CHECK_STR(raw.cmd("resume"), "ok");
    waitStop(" reason=watchpoint access=write space=m68k addr=FF4276 size=4 value=12345678");
    CHECK_STR(raw.cmd("bpclear"), "ok");

    w.emu.parkM68kAt(kJsr);
    CHECK_STR(raw.cmd("frameadv 1"), "ok");
    waitStop(" reason=frame");

    CHECK_STR(raw.cmd("resume"), "ok");
    CHECK_STR(raw.cmd("pause"), "ok");
    waitStop(" reason=pause");
    const std::string st = raw.cmd("status");
    CHECK(st.rfind("ok running=1 paused=1 pc=", 0) == 0);
    CHECK(st.find(" reason=pause") != std::string::npos);
}

TEST(bridge_returns_the_access_log)
{
    Wired w(26);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(26)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    CHECK_STR(raw.cmd("log"), "ok enabled=0 entries=0 dropped=0");
    CHECK(raw.cmd("log on m68k:ff4276").rfind("err usage", 0) == 0);
    CHECK_STR(raw.cmd("log on cap=8 m68k:ff4276-ff4279"), "ok");
    raw.cmd("bpadd x 250 250");
    w.emu.parkM68kAt(kDataEntry);
    CHECK_STR(raw.cmd("resume"), "ok");
    CHECK(raw.cmd("wait 3000").rfind("ok paused pc=250", 0) == 0);
    CHECK_STR(raw.cmd("log"), "ok enabled=1 entries=2 dropped=0");
    CHECK_STR(raw.cmd("log get"),
              "ok total=2 dropped=0 start=0 240,m68k,w,m68k,ff4276,4,12345678 24a,m68k,r,m68k,ff4276,4,12345678");
    CHECK_STR(raw.cmd("log get 1 1"), "ok total=2 dropped=0 start=1 24a,m68k,r,m68k,ff4276,4,12345678");
    CHECK_STR(raw.cmd("log off"), "ok");
    CHECK_STR(raw.cmd("log"), "ok enabled=0 entries=2 dropped=0");
    raw.cmd("bpclear");
}

TEST(bridge_runto_return_address_and_budget)
{
    Wired w(27);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(27)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    CHECK(raw.cmd("runto").rfind("err usage", 0) == 0);
    CHECK(raw.cmd("runto instr=x").rfind("err usage", 0) == 0);

    w.emu.parkM68kAt(kJsr);
    raw.cmd("stepi");
    CHECK(raw.cmd("wait 3000").rfind("ok paused pc=300 ", 0) == 0);
    CHECK_STR(raw.cmd("runto return"), "ok");
    const std::string r = raw.cmd("wait 3000");
    CHECK(r.rfind("ok paused pc=21e ", 0) == 0);
    CHECK(r.find(" reason=return") != std::string::npos);

    CHECK_STR(raw.cmd("runto 302"), "ok");
    const std::string a = raw.cmd("wait 3000");
    CHECK(a.rfind("ok paused pc=302 ", 0) == 0);
    CHECK(a.find(" reason=address") != std::string::npos);

    CHECK_STR(raw.cmd("runto instr=3"), "ok");
    const std::string b = raw.cmd("wait 3000");
    CHECK(b.find(" reason=budget") != std::string::npos);

    CHECK_STR(raw.cmd("runto frames=2"), "ok");
    const std::string f = raw.cmd("wait 3000");
    CHECK(f.find(" reason=budget") != std::string::npos);

    // Issued while running: it pauses first, so the target is not run past.
    CHECK_STR(raw.cmd("resume"), "ok");
    CHECK_STR(raw.cmd("runto 302"), "ok");
    std::string g;
    for (int i = 0; i < 3; ++i) {                        // skip the settling pause
        g = raw.cmd("wait 3000");
        if (g.find(" reason=address") != std::string::npos) break;
    }
    CHECK(g.rfind("ok paused pc=302 ", 0) == 0);
}

// One command runs one case: set registers and memory, run to a stop
// condition, report why it stopped, the registers, the RAM diff, the log.
TEST(bridge_runcase_runs_a_routine_to_its_return)
{
    Wired w(28);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(28)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    CHECK(raw.cmd("runcase return").rfind("err usage", 0) == 0);          // no pc
    CHECK(raw.cmd("runcase pc=300").rfind("err usage", 0) == 0);          // no stop condition

    // nop; rts at $300, returning to $21E pushed at $FFEFFC.
    const std::string r = raw.cmd("runcase pc=300 a7=ffeffc sr=2700 d3=1234 mem=ffeffc:0000021e return");
    CHECK(r.rfind("ok reason=return pc=21e ", 0) == 0);
    CHECK(r.find(" d3=1234 ") != std::string::npos);
    CHECK(r.find(" a7=fff000 ") != std::string::npos);
    CHECK(r.find(" diff=0") != std::string::npos);                      // rts only reads
    CHECK(r.find(" log=-") != std::string::npos);                       // log not requested

    // Started while the machine runs: runcase pauses first, then runs the case.
    CHECK_STR(raw.cmd("resume"), "ok");
    const std::string again = raw.cmd("runcase pc=300 a7=ffeffc sr=2700 mem=ffeffc:0000021e return");
    CHECK(again.rfind("ok reason=return pc=21e ", 0) == 0);
}

TEST(bridge_runcase_reports_ram_diff_and_access_log)
{
    Wired w(29);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(29)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    // The data routine writes $12345678 to $FF4276 then reads it back.
    const std::string r = raw.cmd("runcase pc=240 a7=fff000 mem=ff4276:00000000 until=250 "
                                  "log=m68k:ff4276-ff4279");
    CHECK(r.rfind("ok reason=address pc=250 ", 0) == 0);
    CHECK(r.find(" d0=12345678 ") != std::string::npos);
    CHECK(r.find(" diff=1 4276:12345678 ") != std::string::npos);
    CHECK(r.find(" log=2 dropped=0 240,m68k,w,m68k,ff4276,4,12345678 24a,m68k,r,m68k,ff4276,4,12345678")
          != std::string::npos);

    // A whole 64 KiB RAM image, all $AA, then the same run: the diff is
    // relative to the image, so only the four written bytes differ.
    const std::string img(0x10000 * 2, 'a');
    const std::string i = raw.cmd("runcase pc=240 a7=fff000 ram=" + img + " until=250");
    CHECK(i.find(" diff=1 4276:12345678 ") != std::string::npos);

    const std::string b = raw.cmd("runcase pc=21e a7=fff000 instr=2");
    CHECK(b.rfind("ok reason=budget pc=218 ", 0) == 0);                 // nop, bra.s back to the jsr
}

// Stopped by a watchpoint, the machine is inside an unfinished instruction.
// A case set up there must not be clobbered when that instruction completes.
TEST(bridge_runcase_after_a_watchpoint_stop_keeps_its_registers)
{
    Wired w(32);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(32)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    raw.cmd("bpadd r ff4276 ff4279");
    w.emu.parkM68kAt(kDataEntry);
    CHECK_STR(raw.cmd("resume"), "ok");
    REQUIRE(raw.cmd("wait 3000").find(" reason=watchpoint access=read") != std::string::npos);
    raw.cmd("bpclear");

    const std::string r = raw.cmd("runcase pc=300 a7=fff000 sr=2700 d0=deadbeef until=302");
    CHECK(r.rfind("ok reason=address pc=302 ", 0) == 0);
    CHECK(r.find(" d0=deadbeef ") != std::string::npos);
}

// One MOVEM trips a range watchpoint once per register: many stops inside a
// single instruction. runcase must still reach its end before applying a case.
TEST(bridge_runcase_after_many_watchpoint_stops_in_one_instruction)
{
    Wired w(33);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(33)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    // In work RAM: movem.l d0-d7/a0-a6,($FF4000).l ; bra.s *
    const uint8_t code[] = { 0x48, 0xF9, 0x7F, 0xFF, 0x00, 0xFF, 0x40, 0x00, 0x60, 0xFE };
    w.emu.backend()->writeMemory(0xFF0200, code, sizeof code);
    raw.cmd("bpadd w ff4000 ff403f");
    M68kRegs r = w.emu.backend()->getM68kRegs();
    r.pc = 0xFF0200; r.sr = 0x2700; r.a[7] = kInitSp;
    w.emu.backend()->setM68kRegs(r);
    CHECK_STR(raw.cmd("resume"), "ok");
    REQUIRE(raw.cmd("wait 3000").find(" reason=watchpoint access=write") != std::string::npos);

    const std::string c = raw.cmd("runcase pc=300 a7=fff000 sr=2700 d0=deadbeef until=302");
    CHECK(c.rfind("ok reason=address pc=302 ", 0) == 0);
    CHECK(c.find(" d0=deadbeef ") != std::string::npos);
    raw.cmd("bpclear");
}

// A Z80 stop inside a 68000 write (the write synchronised the Z80) leaves that
// write unfinished; completing it would clobber the case's condition codes.
TEST(bridge_runcase_after_a_z80_stop_inside_a_68000_write_keeps_its_flags)
{
    Wired w(34);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(34)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    w.emu.backend()->resume();                               // boot releases the Z80
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(w.emu.pauseAndWait());
    {   // a few 68000 instructions into a frame: the Z80 (run after the 68000
        // in each line) lags, so the write below runs Z80 instructions
        const uint64_t f = w.emu.pauseSeq();
        w.emu.gpgx()->requestFramePause();
        w.emu.backend()->resume();
        REQUIRE(w.emu.waitPause(f));
        RunLimits few;
        few.maxInstructions = 20;
        const uint64_t g = w.emu.pauseSeq();
        w.emu.gpgx()->armRun(few);
        w.emu.backend()->resume();
        REQUIRE(w.emu.waitPause(g));
    }
    Breakpoint zbp;
    zbp.type = BpType::PC; zbp.cpu = Cpu::Z80; zbp.start = 0; zbp.end = 0xFFFF;   // anywhere
    const int id = w.emu.backend()->addBreakpoint(zbp);
    w.emu.parkM68kAt(kZ80BusWait + 8);                       // move.w #$100,$A11100
    const uint64_t s = w.emu.pauseSeq();
    w.emu.backend()->resume();
    REQUIRE(w.emu.waitPause(s));
    w.emu.backend()->removeBreakpoint(id);
    REQUIRE(w.emu.gpgx()->stopInsideM68kInstruction());

    const std::string r = raw.cmd("runcase pc=300 a7=fff000 sr=271f until=302");
    CHECK(r.rfind("ok reason=address pc=302 ", 0) == 0);
    CHECK(r.find(" sr=271f ") != std::string::npos);
}

// a7= names the stack of the case's own mode, even when it equals the current
// (other-bank) stack pointer.
TEST(bridge_runcase_a7_follows_the_cases_sr_bank)
{
    Wired w(35);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(35)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    // Supervisor, ISP = $FFF000, USP = 0.
    REQUIRE(raw.cmd("runcase pc=300 sr=2700 a7=fff000 mem=fff000:0000021e return").rfind("ok reason=return", 0) == 0);
    REQUIRE(raw.cmd("wreg68k usp=0") == "ok");
    REQUIRE(raw.cmd("wreg68k a7=fff000") == "ok");
    // User mode, same A7 value: the user stack must be $FFF000 for the case.
    const std::string r = raw.cmd("runcase pc=300 sr=0000 a7=fff000 mem=fff000:0000021e return");
    CHECK(r.rfind("ok reason=return pc=21e ", 0) == 0);
    CHECK(r.find(" a7=fff004 ") != std::string::npos);
    CHECK(r.find(" usp=fff004 ") != std::string::npos);
}

TEST(bridge_runcase_from_a_save_state_is_deterministic)
{
    Wired w(30);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(30)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : ".") + "/smd_dgx_runcase.state";
    REQUIRE(raw.cmd("savestate " + path) == "ok");
    const std::string c = "runcase state=" + path + " pc=218 a7=fff000 frames=2";
    const std::string first = raw.cmd(c);
    // Stop somewhere else in a frame before running the same case again: the
    // result must not depend on where the previous run left the scheduler.
    CHECK_STR(raw.cmd("runto instr=37"), "ok");
    raw.cmd("wait 3000");
    const std::string second = raw.cmd(c);
    CHECK(first.rfind("ok reason=budget ", 0) == 0);
    CHECK_STR(second, first);

    // Timing-sensitive: read the VDP H/V counter (move.w $C00008,d0; bra.s *).
    const std::string hv = "runcase state=" + path + " pc=ff0100 a7=fff000 sr=2700 "
                           "mem=ff0100:303900c0000860fe until=ff0106";
    const std::string h1 = raw.cmd(hv);
    CHECK_STR(raw.cmd("runto instr=53"), "ok");
    raw.cmd("wait 3000");
    const std::string h2 = raw.cmd(hv);
    CHECK(h1.rfind("ok reason=address pc=ff0106 ", 0) == 0);
    CHECK_STR(h2, h1);
    std::remove(path.c_str());
}

TEST(bridge_override_lets_a_z80_bus_wait_finish)
{
    Wired w(31);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(31)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    CHECK_STR(raw.cmd("override"), "ok n=0");
    CHECK(raw.cmd("override io a11100 3 1 0").rfind("err usage", 0) == 0);
    const std::string c = "runcase pc=560 a7=ffeffc sr=2700 mem=ffeffc:0000021e return instr=20000";
    CHECK(raw.cmd(c).rfind("ok reason=budget ", 0) == 0);              // spins: Z80 in reset
    CHECK_STR(raw.cmd("override z80bus"), "ok n=2 a11100:1:1:0 a11100:2:100:0");
    CHECK(raw.cmd(c).rfind("ok reason=return pc=21e ", 0) == 0);
    CHECK_STR(raw.cmd("override vdp 88 80"),
              "ok n=8 a11100:1:1:0 a11100:2:100:0 c00004:2:88:80 c00004:1:0:0 c00005:1:88:80 "
              "c00006:2:88:80 c00006:1:0:0 c00007:1:88:80");
    CHECK_STR(raw.cmd("override none"), "ok n=0");
    CHECK(raw.cmd(c).rfind("ok reason=budget ", 0) == 0);
}

TEST(bridge_falls_through_to_the_backend_without_a_host)
{
    // The standalone app installs nothing, and a host may decline any single
    // operation — a Z80 breakpoint means nothing in a 68000 database, so IDA's
    // handler returns false for those and the backend must still get it.
    Wired w(18);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(18)));

    EmuHost::HostMutations hm;
    hm.addBreakpoint = [](const Breakpoint&, int*) { return false; };  // declines
    w.emu.host()->setHostMutations(hm);

    const std::string r = raw.cmd("bpadd x 300 300 cpu=z80");
    CHECK(r.rfind("ok ", 0) == 0);
    REQUIRE(w.emu.backend()->getBreakpoints().size() == 1);
    CHECK(w.emu.backend()->getBreakpoints()[0].cpu == Cpu::Z80);

    w.emu.backend()->clearBreakpoints();
    w.emu.host()->setHostMutations({});
}

// ---------------------------------------------------------------------------
// Saying "ok" to input you did not understand.
//
// The dispatcher used to parse numbers with strtoul and a null endptr, which
// cannot fail: a missing argument became 0, and a half-numeric one was
// truncated. Every case below was hit while driving a real ROM, and each one
// produced plausible-looking wrong data rather than an error.
// ---------------------------------------------------------------------------
TEST(bridge_bpadd_refuses_a_malformed_command)
{
    Wired w(19);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(19)));

    // The reported case: an address where the type belongs. This used to answer
    // "ok 1" and set a breakpoint at 0 that could never fire.
    CHECK(raw.cmd("bpadd 74e0").rfind("err", 0) == 0);
    // ...and no arguments at all.
    CHECK(raw.cmd("bpadd").rfind("err", 0) == 0);
    // A junk type, and a junk address.
    CHECK(raw.cmd("bpadd q 74e0").rfind("err", 0) == 0);
    CHECK(raw.cmd("bpadd x zzz").rfind("err", 0) == 0);
    // Nothing was created by any of them.
    CHECK(w.emu.backend()->getBreakpoints().empty());

    // The correct form still works, with and without an explicit end.
    CHECK(raw.cmd("bpadd x 74e0").rfind("ok ", 0) == 0);
    CHECK(raw.cmd("bpadd w 74e0 74e3 cpu=m68k").rfind("ok ", 0) == 0);

    const auto bps = w.emu.backend()->getBreakpoints();
    REQUIRE(bps.size() == 2);
    CHECK_EQ(bps[0].start, 0x74E0u);
    CHECK_EQ(bps[0].end,   0x74E0u);      // end defaults to start, not to 0
    CHECK_EQ(bps[1].end,   0x74E3u);

    CHECK(raw.cmd("bpdel").rfind("err", 0) == 0);   // id is required too
    w.emu.backend()->clearBreakpoints();
}

TEST(bridge_rejects_a_number_with_trailing_garbage)
{
    Wired w(20);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(20)));

    // The reported case: a decimal length of "6c" silently became 6, so the
    // caller got a six-byte buffer and no hint that anything was wrong.
    CHECK(raw.cmd("readregion 1 212a 6c").rfind("err", 0) == 0);
    // A plain decimal length still works...
    CHECK_EQ(raw.cmd("readregion 1 21d8 64").size(), size_t(3 + 64 * 2));
    // ...and 0x is the escape hatch for callers who think in hex.
    CHECK_EQ(raw.cmd("readregion 1 21d8 0x40").size(), size_t(3 + 64 * 2));

    // The same parser guards every command that takes a number.
    CHECK(raw.cmd("read 21d8 6c").rfind("err", 0) == 0);
    CHECK(raw.cmd("readregion").rfind("err", 0) == 0);
    CHECK(raw.cmd("snap").rfind("err", 0) == 0);        // id 0 is ROM: a real
    CHECK(raw.cmd("search").rfind("err", 0) == 0);      // region, so defaulting
    CHECK(raw.cmd("diff").rfind("err", 0) == 0);        // it searched the wrong one
    CHECK(raw.cmd("setvdpreg 99 00").rfind("err", 0) == 0);
}

// A frame-stepping loop must be able to tell a new stop from the one it was
// already standing on. Without that, "resume then poll until paused" returns
// immediately and the caller re-reads the same frame — in a real trace this
// produced runs of identical samples that looked entirely plausible.
TEST(bridge_status_counters_distinguish_a_new_stop)
{
    Wired w(21);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(21)));

    auto field = [](const std::string& s, const char* key) -> unsigned long long {
        const size_t p = s.find(key);
        if (p == std::string::npos) return 0;
        return std::strtoull(s.c_str() + p + std::strlen(key), nullptr, 10);
    };

    const std::string s0 = raw.cmd("status");
    CHECK(s0.find("stops=") != std::string::npos);
    CHECK(s0.find("frames=") != std::string::npos);
    const unsigned long long stops0 = field(s0, "stops=");

    // resume must not return while still paused, or the status right after it
    // reports the stale value.
    CHECK_STR(raw.cmd("resume"), "ok");
    CHECK(raw.cmd("status").find("paused=0") != std::string::npos);

    // One frame, synchronously — the whole poll dance exists because this was
    // missing.
    const std::string f = raw.cmd("stepframe");
    REQUIRE(f.rfind("ok", 0) == 0);
    const unsigned long long stops1 = field(f, "stops=");
    CHECK(stops1 > stops0);                       // a genuinely new stop
    CHECK(w.emu.backend()->isPaused());

    const std::string f2 = raw.cmd("stepframe");
    REQUIRE(f2.rfind("ok", 0) == 0);
    CHECK(field(f2, "stops=")  > stops1);
    CHECK(field(f2, "frames=") > field(f, "frames="));

    CHECK(raw.cmd("stepframe zz").rfind("err", 0) == 0);
}

TEST(bridge_rejects_absurd_sizes_instead_of_allocating)
{
    Wired w(9);
    REQUIRE(w.ok);

    // A client asking for 4 GB must get an error, not an out-of-memory abort in
    // the emulator hosting someone's IDA session.
    const auto huge = w.remote.readMemory(0, 0xFFFFFFFFu);
    CHECK(huge.empty());
    const auto zero = w.remote.readMemory(0, 0);
    CHECK(zero.empty());
}

// ---------------------------------------------------------------------------
// Run control with a host (IDA) installed. The host keeps its own model of the
// run state, so every bridge command that runs the machine must go through the
// host's resume hook: one that resumes the backend behind its back leaves IDA
// believing in a stop that is long gone (the pocahontas-decomp wedge).
// ---------------------------------------------------------------------------
TEST(bridge_runs_the_machine_only_through_the_host)
{
    Wired w(36);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(36)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    std::atomic<int> resumes{0};
    EmuHost::HostMutations hm;
    hm.resume = [&] { ++resumes; w.emu.backend()->resume(); return true; };
    w.emu.host()->setHostMutations(hm);

    auto ran = [&](const std::string& c, bool waitStop) {
        const int before = resumes.load();
        const std::string r = raw.cmd(c);
        if (waitStop) raw.cmd("wait 3000");
        return r.rfind("ok", 0) == 0 && resumes.load() == before + 1;
    };
    CHECK(ran("frameadv 1", true));
    CHECK(ran("stepframe 1", false));
    CHECK(ran("stepi", true));
    CHECK(ran("stepo", true));
    CHECK(ran("runto instr=10", true));
    CHECK(ran("resume", false));
    raw.cmd("pause");
    raw.cmd("wait 3000");
    w.emu.host()->setHostMutations({});
}

// A pause must reach the emulator whatever the host believes about it: a host
// that thinks the process is already suspended used to swallow it, and the
// emulator ran on with every later command failing "could not pause".
TEST(bridge_pause_stops_a_running_emulator_whatever_the_host_believes)
{
    Wired w(37);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(37)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    // The host used to be asked first, and a host that believed the process
    // already suspended (IDA after a stale stop) answered "done" and did
    // nothing. There is no such hook any more; the host is still installed.
    EmuHost::HostMutations hm;
    hm.resume = [&] { w.emu.backend()->resume(); return true; };
    w.emu.host()->setHostMutations(hm);

    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    CHECK_STR(raw.cmd("pause"), "ok");
    CHECK(raw.cmd("wait 2000").rfind("ok paused", 0) == 0);
    CHECK(w.emu.backend()->isPaused());
    // runcase settles first: it must not fail with "could not pause".
    CHECK(raw.cmd("resume").rfind("ok", 0) == 0);
    const std::string r = raw.cmd("runcase pc=300 sr=2700 a7=fff000 mem=fff000:0000021e return");
    CHECK(r.rfind("ok reason=return", 0) == 0);
    w.emu.host()->setHostMutations({});
}

// A host that cannot take the resume (IDA busy, shutting down) must get an
// error back, never a resume behind its back.
TEST(bridge_never_resumes_behind_a_host_that_declined)
{
    Wired w(38);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(38)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    EmuHost::HostMutations hm;
    hm.resume = [] { return false; };
    w.emu.host()->setHostMutations(hm);
    CHECK(raw.cmd("resume").rfind("err", 0) == 0);
    CHECK(raw.cmd("frameadv 1").rfind("err", 0) == 0);
    CHECK(raw.cmd("stepi").rfind("err", 0) == 0);
    CHECK(w.emu.backend()->isPaused());
    w.emu.host()->setHostMutations({});
}

// ---------------------------------------------------------------------------
// The IDA plugin's run-control glue, against the real emulator, with IDA's main
// thread modelled: requests posted to it (the continue) and events it polls run
// only when the test "frees" it, as when a headless script stops pumping.
// ---------------------------------------------------------------------------
namespace {
struct FakeIda {
    HostRunSync<DebugEvent> sync;
    IDebugBackend* be;
    std::mutex mx;
    std::vector<std::function<void()>> mainQueue;   // execute_sync NOWAIT
    bool suspended = false;                          // DSTATE_SUSP
    uint32_t pc = 0;                                 // where IDA believes it stopped
    int sink = -1;
    EmuHost* host;
    std::condition_variable gateCv;
    bool gateClosed = false;
    void closeGate() { std::lock_guard<std::mutex> lk(mx); gateClosed = true; }
    void openGate()  { { std::lock_guard<std::mutex> lk(mx); gateClosed = false; } gateCv.notify_all(); }
    void pushStop(const DebugEvent& e) { sync.pushStop(e, e.runEpoch); }

    explicit FakeIda(EmuHost* h) : be(h->backend()), host(h)
    {
        sync.setPostContinue([this] { postContinue(); });
        sync.setEpochSource([this] { return host->gpgx()->runEpoch(); });
        sink = h->addEventSink([this](const DebugEvent& e) {
            if (e.type != DebugEvent::Type::Paused) return;
            // A test may hold the publication back, as a slow sink would.
            std::unique_lock<std::mutex> lk(mx);
            gateCv.wait(lk, [this] { return !gateClosed; });
            lk.unlock();
            pushStop(e);
        });
        EmuHost::HostMutations hm;
        hm.resume = [this] {
            sync.run([this] { host->gpgx()->startRunning(); },
                     [this] { if (betweenRunSteps) betweenRunSteps(); host->gpgx()->announceResumed(); });
            return true;
        };
        h->setHostMutations(hm);
    }
    ~FakeIda() { host->setHostMutations({}); host->removeEventSink(sink); }

    void postContinue()                              // post_continue()
    {
        std::lock_guard<std::mutex> lk(mx);
        mainQueue.push_back([this] { if (sync.continueWanted() && suspended) idaContinue(); });
    }
    // Runs between IDA's decision to drive the machine and the resume itself,
    // to test that interleaving.
    std::function<void()> betweenDecisionAndResume;
    std::function<void()> betweenRunSteps;           // inside a bridge run, see below
    void idaContinue()                               // continue_process -> ev_resume
    {
        suspended = false;
        uint64_t epoch = 0;
        const bool drive = sync.releaseHold([this] { return host->gpgx()->beginRun(); }, &epoch);
        if (betweenDecisionAndResume) betweenDecisionAndResume();
        if (drive) host->gpgx()->resumeIfEpoch(epoch);
    }
    // IDA's main thread gets a turn: posted requests, then event processing.
    void freeMainThread()
    {
        std::vector<std::function<void()>> q;
        { std::lock_guard<std::mutex> lk(mx); q.swap(mainQueue); }
        for (auto& f : q) f();
        DebugEvent e; bool more = true;
        while (!suspended && sync.retrieve(&e, &more)) {
            if (e.type != DebugEvent::Type::Paused) continue;
            suspended = true;
            pc = e.pc & 0xFFFFFF;
            if (sync.continueWanted()) postContinue();   // dbg_suspend_process
        }
    }
};
}

TEST(bridge_never_waits_for_a_busy_host_and_the_host_catches_up_without_stale_stops)
{
    Wired w(39);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(39)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    FakeIda ida(w.emu.host());

    // Bring IDA to a stop it holds.
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    REQUIRE(raw.cmd("pause") == "ok");
    REQUIRE(raw.cmd("wait 3000").rfind("ok paused", 0) == 0);
    ida.freeMainThread();
    REQUIRE(ida.suspended);

    // IDA's main thread is now busy. Every run-control command must still
    // answer at once, and the machine must do what it was told.
    auto timed = [&](const std::string& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string r = raw.cmd(c);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 1500);
        return r;
    };
    for (int i = 0; i < 20; ++i) {
        CHECK(timed("frameadv 1") == "ok");
        CHECK(timed("wait 3000").rfind("ok paused", 0) == 0);
        CHECK(timed("stepi") == "ok");
        CHECK(timed("wait 3000").rfind("ok paused", 0) == 0);
        CHECK(timed("stepframe 1").rfind("ok", 0) == 0);
        CHECK(timed("resume").rfind("ok", 0) == 0);
        CHECK(timed("pause") == "ok");
        CHECK(timed("wait 3000").rfind("ok paused", 0) == 0);
        CHECK(timed("runcase pc=300 sr=2700 a7=fff000 mem=fff000:0000021e return").rfind("ok reason=return", 0) == 0);
        const std::string hang = timed("runcase pc=220 sr=2700 a7=fff000 mem=fff000:0000021e return timeout=50");
        if (hang.find(" timeout=1") == std::string::npos) std::printf("    hang reply: %s\n", hang.substr(0, 160).c_str());
        CHECK(hang.find(" timeout=1") != std::string::npos);
        CHECK(w.emu.backend()->isPaused());
    }
    // IDA is still suspended where it was (it could not look), with exactly one
    // continue waiting for it.
    CHECK(ida.suspended);
    { std::lock_guard<std::mutex> lk(ida.mx); CHECK_EQ(ida.mainQueue.size(), size_t(1)); }

    // Its main thread frees up: it continues, finds the machine already moved
    // on (and does not run it from there), and suspends at the current stop.
    const uint32_t emuPc = w.emu.backend()->getM68kRegs().pc & 0xFFFFFF;
    ida.freeMainThread();
    CHECK(ida.suspended);
    CHECK_EQ(ida.pc, emuPc);
    CHECK(w.emu.backend()->isPaused());

    // And IDA's own run control still works from there.
    ida.idaContinue();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!w.emu.backend()->isPaused());
    w.emu.backend()->pause();                        // suspend_process -> ev_request_pause
    REQUIRE(raw.cmd("wait 3000").rfind("ok paused", 0) == 0);
    // The stop reaches the bridge's and IDA's sinks one after the other.
    for (int i = 0; i < 200 && !ida.suspended; ++i) {
        ida.freeMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(ida.suspended);
    CHECK_EQ(ida.pc, w.emu.backend()->getM68kRegs().pc & 0xFFFFFF);
}

// ---------------------------------------------------------------------------
// Stale stop requests: a pause, step or frame-end request that another stop
// overtook (or that was made on a machine already paused) must not stop some
// later, unrelated run.
// ---------------------------------------------------------------------------
TEST(stale_pause_on_a_paused_machine_arms_nothing)
{
    Wired w(40);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(40)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    REQUIRE(raw.cmd("pause") == "ok");                   // already paused
    REQUIRE(raw.cmd("pause") == "ok");
    const std::string r = raw.cmd("runcase pc=300 sr=2700 a7=fff000 mem=fff000:0000021e return");
    CHECK(r.rfind("ok reason=return", 0) == 0);
    const std::string t = raw.cmd("runto instr=20");
    CHECK(t == "ok");
    const std::string ws = raw.cmd("wait 3000");
    if (ws.find(" reason=budget") == std::string::npos) std::printf("    wait: %s\n", ws.c_str());
    CHECK(ws.find(" reason=budget") != std::string::npos);
}

TEST(stale_step_cut_short_by_a_watchpoint_leaves_nothing_armed)
{
    Wired w(41);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(41)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    M68kRegs r = w.emu.backend()->getM68kRegs();
    r.pc = kDataEntry; r.sr = 0x2700; r.a[7] = kInitSp;
    w.emu.backend()->setM68kRegs(r);
    const std::string add = raw.cmd("bpadd w ff4276");
    REQUIRE(add.rfind("ok ", 0) == 0);
    REQUIRE(raw.cmd("stepi") == "ok");                   // the write trips the watchpoint
    REQUIRE(raw.cmd("wait 3000").find(" reason=watchpoint") != std::string::npos);
    REQUIRE(raw.cmd("bpdel " + add.substr(3)) == "ok");
    REQUIRE(raw.cmd("runto instr=5") == "ok");
    CHECK(raw.cmd("wait 3000").find(" reason=budget") != std::string::npos);
}

TEST(stale_frame_advance_cut_short_is_dropped_by_the_next_run)
{
    Wired w(42);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(42)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    const std::string add = raw.cmd("bpadd x 300");
    REQUIRE(add.rfind("ok ", 0) == 0);
    REQUIRE(raw.cmd("frameadv 5") == "ok");              // a breakpoint comes first
    REQUIRE(raw.cmd("wait 3000").find(" reason=breakpoint") != std::string::npos);
    REQUIRE(raw.cmd("bpdel " + add.substr(3)) == "ok");
    // A run-to command starts afresh: the advance's remaining frames are gone.
    const std::string c = raw.cmd("runcase pc=218 sr=2700 a7=fff000 instr=200000 timeout=5000");
    CHECK(c.rfind("ok reason=budget", 0) == 0);
    // ...while a plain resume after the breakpoint still finishes an advance.
    const std::string add2 = raw.cmd("bpadd x 300");
    REQUIRE(raw.cmd("frameadv 3") == "ok");
    const std::string w2 = raw.cmd("wait 3000");
    if (w2.find(" reason=breakpoint") == std::string::npos) std::printf("    wait: %s\n", w2.c_str());
    REQUIRE(w2.find(" reason=breakpoint") != std::string::npos);
    REQUIRE(raw.cmd("bpdel " + add2.substr(3)) == "ok");
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    CHECK(raw.cmd("wait 3000").find(" reason=frame") != std::string::npos);
}

TEST(stale_frame_boundary_request_overtaken_by_a_breakpoint_is_dropped)
{
    Wired w(43);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(43)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    const std::string add = raw.cmd("bpadd x 300");
    REQUIRE(add.rfind("ok ", 0) == 0);
    w.emu.gpgx()->requestFramePause();                   // as runcase state= does
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    REQUIRE(raw.cmd("wait 3000").find(" reason=breakpoint") != std::string::npos);
    REQUIRE(raw.cmd("bpdel " + add.substr(3)) == "ok");
    const std::string c = raw.cmd("runcase pc=218 sr=2700 a7=fff000 instr=200000 timeout=5000");
    CHECK(c.rfind("ok reason=budget", 0) == 0);
}

// ---------------------------------------------------------------------------
// An armed run whose 68000 stops executing (double fault, STOP with every
// interrupt masked) can never reach its return, address or instruction budget:
// it ends at once, saying why, instead of running out the wall clock.
// ---------------------------------------------------------------------------
namespace {
std::string hex(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%x", unsigned(v));
    return b;
}
long long msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}
}

TEST(armed_run_ends_promptly_when_the_68000_halts)
{
    Wired w(44);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(44)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string r = raw.cmd(
        "runcase pc=" + hex(kHaltEntry) + " sr=2700 a7=fff000 mem=fff000:0000021e return instr=1000000 timeout=8000");
    const long long ms = msSince(t0);
    CHECK(r.rfind("ok reason=halted", 0) == 0);
    CHECK(r.find(" timeout=1") == std::string::npos);
    CHECK(ms < 2000);
    // runto too.
    M68kRegs regs = w.emu.backend()->getM68kRegs();
    regs.pc = kHaltEntry; regs.sr = 0x2700; regs.a[7] = kInitSp;
    w.emu.backend()->setM68kRegs(regs);
    REQUIRE(raw.cmd("runto instr=1000000") == "ok");
    CHECK(raw.cmd("wait 2000").find(" reason=halted") != std::string::npos);
}

TEST(armed_run_ends_promptly_when_the_68000_stops_for_good)
{
    Wired w(45);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(45)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string r = raw.cmd(
        "runcase pc=" + hex(kStopEntry) + " sr=2700 a7=fff000 mem=fff000:0000021e return instr=1000000 timeout=8000");
    CHECK(r.rfind("ok reason=stopped", 0) == 0);
    CHECK(r.find(" timeout=1") == std::string::npos);
    CHECK(msSince(t0) < 2000);
}

// A busy-wait on the Z80 bus still executes instructions: it is not "stopped",
// and its instruction budget ends it, promptly.
TEST(armed_run_on_a_z80_bus_wait_ends_on_its_budget)
{
    Wired w(46);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(46)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string r = raw.cmd(
        "runcase pc=" + hex(kZ80BusWait) + " sr=2700 a7=fff000 mem=fff000:0000021e return instr=20000 timeout=8000");
    CHECK(r.rfind("ok reason=budget", 0) == 0);
    CHECK(msSince(t0) < 2000);
}

// ---------------------------------------------------------------------------
// Speed: real time by default (a person is watching), unpaced on request (a
// batch client is not). Same machine either way: only the wall clock differs.
// ---------------------------------------------------------------------------
TEST(speed_max_runs_unpaced_with_identical_results)
{
    Wired w(47);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(47)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);

    CHECK_STR(raw.cmd("speed"), "ok speed=realtime");
    CHECK(raw.cmd("status").find(" speed=realtime") != std::string::npos);
    CHECK(raw.cmd("speed warp").rfind("err usage", 0) == 0);

    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : ".") + "/smd_dgx_speed.state";
    REQUIRE(raw.cmd("savestate " + path) == "ok");
    const std::string c = "runcase state=" + path + " pc=218 sr=2700 a7=fff000 frames=60 log=m68k:fff000-ffffff logcap=32";

    auto timedFrames = [&](int n) -> long long {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(raw.cmd("frameadv " + std::to_string(n)) == "ok");
        CHECK(raw.cmd("wait 10000").find(" reason=frame") != std::string::npos);
        return msSince(t0);
    };
    const long long paced = timedFrames(120);                 // ~2 s at 60 Hz
    auto t0 = std::chrono::steady_clock::now();
    const std::string pacedCase = raw.cmd(c);
    const long long pacedCaseMs = msSince(t0);

    CHECK_STR(raw.cmd("speed max"), "ok speed=max");
    CHECK(raw.cmd("status").find(" speed=max") != std::string::npos);
    const long long fast = timedFrames(120);
    t0 = std::chrono::steady_clock::now();
    const std::string fastCase = raw.cmd(c);
    const long long fastCaseMs = msSince(t0);
    CHECK_STR(raw.cmd("speed realtime"), "ok speed=realtime");

    std::printf("    frameadv 120: %lld ms paced, %lld ms max; runcase: %lld ms paced, %lld ms max\n",
                paced, fast, pacedCaseMs, fastCaseMs);
    CHECK(paced >= 1500);
    CHECK(fast * 4 < paced);                                  // several times faster
    CHECK(fastCaseMs * 4 < pacedCaseMs);
    CHECK(pacedCase.rfind("ok reason=budget", 0) == 0);
    CHECK_STR(fastCase, pacedCase);                           // same registers, diff, log
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Breakpoint cost: a recorder arms an execute breakpoint on every routine entry
// it still needs (the pocahontas-decomp one wanted thousands). Checking them
// must not cost a scan of the whole list on every instruction and every memory
// access, or the machine crawls exactly when it is being recorded.
// ---------------------------------------------------------------------------
TEST(many_execute_breakpoints_cost_little)
{
    Wired w(48);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(48)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    REQUIRE(raw.cmd("speed max") == "ok speed=max");

    auto frames = [&](int n) -> long long {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(raw.cmd("frameadv " + std::to_string(n)) == "ok");
        CHECK(raw.cmd("wait 60000").find(" reason=frame") != std::string::npos);
        return msSince(t0);
    };
    frames(60);                                          // warm up
    const long long none = frames(600);
    // 1500 breakpoints on addresses the program never executes, as on routines
    // that have not run yet.
    for (int i = 0; i < 1500; ++i)
        REQUIRE(raw.cmd("bpadd x " + hex(0x1000 + 2 * i)).rfind("ok ", 0) == 0);
    const long long many = frames(600);
    std::printf("    600 frames: %lld ms without breakpoints, %lld ms with 1500\n", none, many);
    CHECK(many < none * 2 + 50);
    // They still work: one on the loop is hit at once.
    REQUIRE(raw.cmd("bpadd x 218").rfind("ok ", 0) == 0);
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    CHECK(raw.cmd("wait 3000").find(" reason=breakpoint") != std::string::npos);
    CHECK_EQ(w.emu.backend()->getM68kRegs().pc & 0xFFFFFF, 0x218u);
    raw.cmd("bpclear");
    raw.cmd("speed realtime");
}

// A stop is published to the host by a sink that runs after the machine is
// already marked paused. A bridge client that sees the stop first and runs the
// machine on must not leave the host to collect that stop afterwards.
TEST(a_stop_published_late_is_not_collected_after_the_machine_ran_on)
{
    Wired w(49);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(49)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    FakeIda ida(w.emu.host());
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);      // running; IDA running too
    ida.closeGate();                                      // IDA's sink will be slow
    REQUIRE(raw.cmd("pause") == "ok");
    REQUIRE(raw.cmd("wait 3000").rfind("ok paused", 0) == 0);   // the bridge has it
    std::thread resumer([&] { raw.cmd("resume"); });      // run on before IDA's copy lands
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ida.openGate();
    resumer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ida.freeMainThread();
    CHECK(!w.emu.backend()->isPaused());
    CHECK(!ida.suspended);                                // never on the stale stop
    REQUIRE(raw.cmd("pause") == "ok");
    REQUIRE(raw.cmd("wait 3000").rfind("ok paused", 0) == 0);
    for (int i = 0; i < 200 && !ida.suspended; ++i) {
        ida.freeMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(ida.suspended);
    CHECK_EQ(ida.pc, w.emu.backend()->getM68kRegs().pc & 0xFFFFFF);
}

// IDA's own continue decides to drive the machine, and a bridge client runs it
// to a newer stop before IDA's resume lands: IDA must not run past that stop.
TEST(a_host_continue_racing_a_bridge_run_does_not_run_past_the_newer_stop)
{
    Wired w(50);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(50)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    FakeIda ida(w.emu.host());
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    REQUIRE(raw.cmd("pause") == "ok");
    REQUIRE(raw.cmd("wait 3000").rfind("ok paused", 0) == 0);
    for (int i = 0; i < 200 && !ida.suspended; ++i) {
        ida.freeMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(ida.suspended);                               // IDA holds the stop
    ida.betweenDecisionAndResume = [&] {
        REQUIRE(raw.cmd("stepi") == "ok");                // the bridge runs one step...
        REQUIRE(raw.cmd("wait 3000").find(" reason=step") != std::string::npos);
    };
    ida.idaContinue();                                    // ...then IDA's resume lands
    ida.betweenDecisionAndResume = nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(w.emu.backend()->isPaused());                   // still at the step's stop
    for (int i = 0; i < 200 && !ida.suspended; ++i) {
        ida.freeMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(ida.suspended);                                 // and IDA learns of it
    CHECK_EQ(ida.pc, w.emu.backend()->getM68kRegs().pc & 0xFFFFFF);
}

// A bridge resume landing while the emulator is publishing a stop (epoch taken,
// not yet marked paused) must not leave it paused at a stop IDA discards.
TEST(a_resume_inside_stop_publication_never_hides_the_stop)
{
    Wired w(51);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(51)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    FakeIda ida(w.emu.host());
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);
    std::thread resumer;
    std::atomic<bool> once{false};
    w.emu.gpgx()->setStopSectionHookForTest([&](int phase) {
        if (phase != 0 || once.exchange(true)) return;
        // A bridge client's resume, from its own thread, right now.
        resumer = std::thread([&] { ida.host->hostMutations().resume(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    w.emu.backend()->pause();
    for (int i = 0; i < 200 && !once.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(once.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (resumer.joinable()) resumer.join();
    w.emu.gpgx()->setStopSectionHookForTest(nullptr);
    for (int i = 0; i < 40; ++i) {
        ida.freeMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Either the machine runs (the resume came after the stop) or it is paused
    // and IDA has the stop. Never paused with IDA still believing it runs.
    if (w.emu.backend()->isPaused()) {
        CHECK(ida.suspended);
        CHECK_EQ(ida.pc, w.emu.backend()->getM68kRegs().pc & 0xFFFFFF);
    } else {
        CHECK(!ida.suspended);
    }
    CHECK(raw.cmd("pause") == "ok");
    CHECK(raw.cmd("wait 3000").rfind("ok paused", 0) == 0);
}

// A bridge run on a machine that is still running: a stop may happen, and IDA
// collect it, while that run is being started. IDA must not be left suspended
// at it while the machine runs on.
TEST(a_stop_collected_while_a_bridge_run_starts_is_not_run_past)
{
    Wired w(52);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(52)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    FakeIda ida(w.emu.host());
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);      // running, IDA running
    ida.betweenRunSteps = [&] {
        ida.betweenRunSteps = nullptr;
        // The machine stops now and IDA collects that stop.
        w.emu.backend()->pause();
        for (int i = 0; i < 400 && !ida.suspended; ++i) {
            ida.freeMainThread();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);      // a second resume, while running
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int i = 0; i < 40; ++i) {
        ida.freeMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (w.emu.backend()->isPaused()) {
        CHECK(ida.suspended);
        CHECK_EQ(ida.pc, w.emu.backend()->getM68kRegs().pc & 0xFFFFFF);
    } else {
        CHECK(!ida.suspended);                            // never suspended on a stop run past
    }
}

// Stepping (or frame-advancing) a machine that is still running: the request
// must not be consumed, and its stop run past, before the host gets to start
// the run. The machine ends paused at the step's (the advance's) stop.
TEST(stepping_a_running_machine_ends_paused_at_the_step)
{
    Wired w(53);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(53)));
    REQUIRE(raw.cmd("ping").rfind("ok", 0) == 0);
    EmuHost::HostMutations hm;
    hm.resume = [&] {                                     // a host slow to start the run
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        w.emu.backend()->resume();
        return true;
    };
    w.emu.host()->setHostMutations(hm);
    for (const char* c : { "stepi", "stepo", "frameadv 1", "stepframe 1" }) {
        REQUIRE(raw.cmd("resume").rfind("ok", 0) == 0);  // running
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(raw.cmd(c).rfind("ok", 0) == 0);
        const std::string cs = c;
        if (cs != "stepframe 1") {                        // the command's own stop, not the settling pause
            const std::string wr = raw.cmd("wait 3000");
            CHECK(wr.find(cs.rfind("step", 0) == 0 ? " reason=step" : " reason=frame") != std::string::npos);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::string st = raw.cmd("status");
        if (st.find(" paused=1") == std::string::npos) std::printf("    %s -> %s\n", c, st.c_str());
        CHECK(st.find(" paused=1") != std::string::npos);
    }
    w.emu.host()->setHostMutations({});
}

// A command that settles before arming must settle on a stop that is fully
// published: a stop that is marked paused but not yet announced still clears
// the pending requests, and would clear the step just armed.
namespace {
// Pause a running machine and, while that stop is being published (phase 1 of
// the seam), issue `cmd` from a second client. Returns that client's reply.
std::string commandDuringStopPublication(Wired& w, int slot, Raw& other, const std::string& cmd)
{
    Raw raw;
    if (!raw.connect(portFor(slot))) return "connect failed";
    raw.cmd("resume");
    std::thread client;
    std::string reply;
    std::atomic<bool> once{false};
    w.emu.gpgx()->setStopSectionHookForTest([&](int phase) {
        if (phase != 1 || once.exchange(true)) return;
        client = std::thread([&] { reply = other.cmd(cmd); });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    });
    w.emu.backend()->pause();
    for (int i = 0; i < 400 && !once.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (client.joinable()) client.join();
    w.emu.gpgx()->setStopSectionHookForTest(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return reply;
}
}

TEST(a_step_armed_while_a_stop_is_being_published_is_not_lost)
{
    Wired w(54);
    REQUIRE(w.ok);
    Raw stepper;
    REQUIRE(stepper.connect(portFor(54)));
    REQUIRE(stepper.cmd("ping").rfind("ok", 0) == 0);
    CHECK(commandDuringStopPublication(w, 54, stepper, "stepi") == "ok");
    CHECK(w.emu.backend()->isPaused());                   // the step ran and stopped
    CHECK(w.emu.gpgx()->lastStopReason() == StopReason::Step);
    // ...and that is the stop wait reports, not the pause that settled first.
    CHECK(stepper.cmd("wait 2000").find(" reason=step") != std::string::npos);

    // stepframe answers when its frame has run, not on the settling pause.
    const std::string f = commandDuringStopPublication(w, 54, stepper, "stepframe 1");
    CHECK(f.rfind("ok", 0) == 0);
    CHECK(w.emu.backend()->isPaused());
    CHECK(w.emu.gpgx()->lastStopReason() == StopReason::FrameAdvance);
}
