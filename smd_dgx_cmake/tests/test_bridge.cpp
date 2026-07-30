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
