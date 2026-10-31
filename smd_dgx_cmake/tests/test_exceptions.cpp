// Every 68000 exception is reported — vector, faulting instruction, stacked SR,
// and for address errors the access — and can optionally stop the debugger on
// the fault itself, before the vector runs.

#include "test_harness.h"
#include "fixture.h"

using namespace synth;

namespace {

constexpr uint64_t bit(unsigned v) { return uint64_t(1) << v; }

// Park at `entry` with a sane supervisor stack and the usual SR, then run.
void runAt(t::Emu& e, uint32_t entry)
{
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = entry;
    r.sr = 0x2700;
    r.a[7] = kInitSp;
    e.backend()->setM68kRegs(r);
    e.backend()->resume();
}

} // namespace

TEST(illegal_instruction_is_reported_without_stopping)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    runAt(e, kIllegalEntry);
    REQUIRE(e.waitExceptions(1));
    const DebugEvent ev = e.exceptions().front();
    CHECK_EQ(unsigned(ev.exc.vector), 4u);
    CHECK_EQ(ev.exc.pc, kIllegalEntry);           // the illegal opcode itself
    CHECK_EQ(ev.exc.framePc, kIllegalEntry);      // illegal stacks its own address
    CHECK_EQ(unsigned(ev.exc.sr), 0x2700u);
    CHECK(ev.exc.access == ExcAccess::None);
    CHECK(!e.backend()->isPaused());              // not caught: it ran on
}

TEST(trap_is_reported_and_can_stop_before_its_vector)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.gpgx()->setExceptionCatch(bit(32));        // trap #0
    const uint64_t s = e.pauseSeq();
    runAt(e, kTrapEntry);
    REQUIRE(e.waitPause(s));
    const DebugEvent p = e.pauseEvent();
    CHECK(p.reason == StopReason::Exception);
    CHECK_EQ(unsigned(p.exc.vector), 32u);
    CHECK_EQ(p.exc.pc, kTrapEntry);
    CHECK_EQ(p.exc.framePc, kTrapEntry + 2);      // trap stacks the next instruction
    CHECK_EQ(p.pc, kTrapEntry);                   // stopped on the fault
    // Resuming runs the vector: the CPU arrives in the handler.
    e.gpgx()->setExceptionCatch(0);
    e.backend()->resume();
    REQUIRE(e.pauseAndWait());
    const uint32_t pc = e.backend()->getM68kRegs().pc;          // in the handler: nop; bra.s *
    CHECK(pc == kExcHandler || pc == kExcHandler + 2);
}

// A debugger stopped on a fault that moves PC has taken over: the vector must
// not then overwrite the new PC.
TEST(moving_pc_while_stopped_on_an_exception_skips_its_vector)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.gpgx()->setExceptionCatch(bit(4));
    const uint64_t s = e.pauseSeq();
    runAt(e, kIllegalEntry);
    REQUIRE(e.waitPause(s));
    REQUIRE(e.pauseReason() == StopReason::Exception);
    e.gpgx()->setExceptionCatch(0);

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K));
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubEntry);
}

TEST(address_error_reports_the_access)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.gpgx()->setExceptionCatch(bit(3));
    const uint64_t s = e.pauseSeq();
    runAt(e, kAddrErrEntry);
    REQUIRE(e.waitPause(s));
    const DebugEvent p = e.pauseEvent();
    CHECK(p.reason == StopReason::Exception);
    CHECK_EQ(unsigned(p.exc.vector), 3u);
    CHECK_EQ(p.exc.pc, kAddrErrEntry);
    CHECK_EQ(p.exc.address, 0x000001u);
    CHECK(p.exc.access == ExcAccess::Read);
    CHECK_EQ(unsigned(p.exc.sr), 0x2700u);
    CHECK(describeStop(p).find("reason=exception vector=3") == 0);
    CHECK_EQ(e.exceptions().size(), size_t(1));   // and reported as an event
}
