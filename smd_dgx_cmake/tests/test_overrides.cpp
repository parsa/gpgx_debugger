// Hardware-wait overrides: opt-in substitutions applied to I/O reads, so a
// routine tested out of context does not spin forever on hardware that is in
// its power-on state. Off by default.

#include "test_harness.h"
#include "fixture.h"

using namespace synth;

namespace {

// Call the Z80 bus wait as a subroutine and run until it returns or a budget
// runs out. Returns the reason the run stopped.
StopReason callBusWait(t::Emu& e)
{
    const uint32_t sp = kInitSp - 4;
    const uint8_t ret[4] = { 0x00, 0x00, uint8_t(kAfterJsr >> 8), uint8_t(kAfterJsr) };
    e.backend()->writeMemory(sp, ret, 4);
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = kZ80BusWait; r.sr = 0x2700; r.a[7] = sp;
    e.backend()->setM68kRegs(r);
    RunLimits lim;
    lim.toReturn = true; lim.returnAddr = kAfterJsr; lim.entrySp = sp;
    lim.maxInstructions = 20000;
    const uint64_t s = e.pauseSeq();
    e.gpgx()->armRun(lim);
    e.backend()->resume();
    if (!e.waitPause(s)) return StopReason::None;
    return e.pauseReason();
}

} // namespace

TEST(z80_bus_wait_spins_without_an_override)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    CHECK(e.gpgx()->readOverrides().empty());          // off by default
    CHECK(callBusWait(e) == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc & ~0xAu, kZ80BusWait + 0x10);   // in the loop
}

TEST(z80_bus_grant_override_lets_the_wait_finish)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    e.gpgx()->setReadOverrides(ReadOverride::z80BusGranted());
    CHECK(callBusWait(e) == StopReason::RunToReturn);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kAfterJsr);

    // And clearing it restores the hardware's answer.
    e.gpgx()->setReadOverrides({});
    CHECK(callBusWait(e) == StopReason::Budget);
}

TEST(vdp_status_override_forces_chosen_bits)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    // In work RAM: move.w $C00004,d0 ; bra.s *
    const uint8_t code[] = { 0x30, 0x39, 0x00, 0xC0, 0x00, 0x04, 0x60, 0xFE };
    e.backend()->writeMemory(0xFF0100, code, sizeof code);
    auto status = [&]() -> uint32_t {
        M68kRegs r = e.backend()->getM68kRegs();
        r.pc = 0xFF0100; r.sr = 0x2700; r.a[7] = kInitSp; r.d[0] = 0;
        e.backend()->setM68kRegs(r);
        RunLimits lim;
        lim.stopAddr = 0xFF0106;
        const uint64_t s = e.pauseSeq();
        e.gpgx()->armRun(lim);
        e.backend()->resume();
        if (!e.waitPause(s)) return 0xFFFFFFFF;
        return e.backend()->getM68kRegs().d[0] & 0xFFFF;
    };

    const uint32_t hw = status();
    REQUIRE(hw != 0xFFFFFFFF);
    e.gpgx()->setReadOverrides(ReadOverride::vdpStatus(0x0088, 0x0080));   // bit 7 set, bit 3 clear
    CHECK_EQ(status() & 0x0088, 0x0080u);
    e.gpgx()->setReadOverrides(ReadOverride::vdpStatus(0x0088, 0x0008));   // the other way round
    CHECK_EQ(status() & 0x0088, 0x0008u);
}

TEST(io_read_override_substitutes_masked_bits)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    // A byte-read override at $A11100 (the BUSACK byte), only bit 0, forced
    // to 0: the same effect as the preset, spelled out.
    e.gpgx()->setReadOverrides({ ReadOverride{ 0xA11100, 1, 0x01, 0x00 } });
    CHECK(callBusWait(e) == StopReason::RunToReturn);
}
