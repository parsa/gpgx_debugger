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
#include "debugger/RemoteBackend.h"
#include "debugger/SocketCompat.h"

#include <algorithm>
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
        int resume = 0, pause = 0, add = 0, del = 0, clear = 0;
    } seen;

    EmuHost::HostMutations hm;
    // A host that claims an operation must actually perform it — IDA reaches
    // the backend through ev_resume; here we stand in for that.
    hm.resume = [&] { ++seen.resume; w.emu.backend()->resume(); return true; };
    hm.pause  = [&] { ++seen.pause;  w.emu.backend()->pause();  return true; };
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
    CHECK_EQ(seen.pause,  1);
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
    const std::string second = raw.cmd(c);
    CHECK(first.rfind("ok reason=budget ", 0) == 0);
    CHECK_STR(second, first);
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
