// A 68000 that executes no instructions — halted after a double fault, or
// waiting in STOP with every interrupt masked — must still be pausable, and
// the stop must say which of the two it is.

#include "test_harness.h"
#include "fixture.h"

#include <chrono>
#include <thread>

using namespace synth;

namespace {

// Run from `entry` until the CPU has certainly stopped executing.
void runInto(t::Emu& e, uint32_t entry)
{
    e.parkM68kAt(entry);
    e.backend()->resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

} // namespace

TEST(pause_lands_on_a_stopped_68000)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    runInto(e, kStopEntry);
    REQUIRE(e.pauseAndWait(1500));
    CHECK_EQ(e.pausePc(), kStopEntry + 4);       // STOP has consumed its operand
    CHECK(e.pauseReason() == StopReason::Stopped);
}

TEST(pause_lands_on_a_halted_68000)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    runInto(e, kHaltEntry);
    REQUIRE(e.pauseAndWait(1500));
    CHECK(e.pauseReason() == StopReason::Halted);
}

TEST(step_on_a_halted_68000_reports_the_halt_instead_of_hanging)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    runInto(e, kHaltEntry);
    REQUIRE(e.pauseAndWait(1500));
    REQUIRE(e.stepAndWait(Cpu::M68K, 1500));
    CHECK(e.pauseReason() == StopReason::Halted);
}

// Setting PC is how a debugger (or a test harness) restarts the CPU somewhere
// else. A halt, or a STOP, must not survive that: the CPU would otherwise
// never run again, and every later pause would report the old halt.
TEST(setting_pc_restarts_a_halted_68000)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    runInto(e, kHaltEntry);
    REQUIRE(e.pauseAndWait(1500));
    REQUIRE(e.pauseReason() == StopReason::Halted);

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K, 1500));
    CHECK(e.pauseReason() == StopReason::Step);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubEntry);
}

TEST(setting_pc_restarts_a_stopped_68000)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    runInto(e, kStopEntry);
    REQUIRE(e.pauseAndWait(1500));
    REQUIRE(e.pauseReason() == StopReason::Stopped);

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K, 1500));
    CHECK(e.pauseReason() == StopReason::Step);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubEntry);
}

// Resuming never stops on the instruction the CPU stands on, whatever kind of
// stop it resumes from (the usual debugger rule: continue steps over a
// breakpoint at the current PC). Moving PC onto a breakpoint and continuing
// therefore runs that instruction, after an in-hook stop and after a
// between-frame stop alike; the breakpoint fires the next time it is reached.
TEST(resume_runs_the_standing_instruction_after_any_kind_of_stop)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    Breakpoint bp;
    bp.type = BpType::PC; bp.start = kJsr; bp.end = kJsr;
    e.backend()->addBreakpoint(bp);
    auto resumeFromJsr = [&]() {
        e.parkM68kAt(kJsr);                              // onto the breakpoint
        const uint64_t s = e.pauseSeq();
        e.backend()->resume();
        REQUIRE(e.waitPause(s));
        CHECK(e.pauseReason() == StopReason::Breakpoint);
        CHECK_EQ(e.pausePc(), kJsr);
        // Reached again through the loop (jsr, sub, nop, bra), not stopped on
        // the standing instruction: the subroutine ran and returned.
        CHECK_EQ(e.backend()->getM68kRegs().a[7], kInitSp);
    };
    resumeFromJsr();                                     // from an in-hook stop

    e.backend()->clearBreakpoints();
    runInto(e, kHaltEntry);
    REQUIRE(e.pauseAndWait(1500));
    REQUIRE(e.pauseReason() == StopReason::Halted);      // a between-frame stop
    e.backend()->addBreakpoint(bp);
    resumeFromJsr();
}

TEST(ordinary_stops_name_their_reason)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    CHECK(e.pauseReason() == StopReason::Pause);

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K));
    CHECK(e.pauseReason() == StopReason::Step);

    Breakpoint bp;
    bp.type = BpType::PC; bp.start = kSubEntry; bp.end = kSubEntry;
    const int id = e.backend()->addBreakpoint(bp);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    CHECK(e.pauseReason() == StopReason::Breakpoint);
    CHECK_EQ(e.pauseEvent().bpId, id);
    e.backend()->clearBreakpoints();

    const uint64_t f = e.pauseSeq();
    e.host()->advanceFrames(2);
    e.backend()->resume();
    REQUIRE(e.waitPause(f));
    CHECK(e.pauseReason() == StopReason::FrameAdvance);
}
