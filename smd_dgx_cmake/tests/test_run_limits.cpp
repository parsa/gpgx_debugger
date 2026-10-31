// Built-in run limits: run until the current routine returns (return address
// AND the stack has unwound past the entry SP), until an address, or until an
// instruction or frame budget runs out — each stop names which.

#include "test_harness.h"
#include "fixture.h"

#include <chrono>
#include <thread>

using namespace synth;

namespace {

bool runAndWait(t::Emu& e, const RunLimits& lim)
{
    const uint64_t s = e.pauseSeq();
    e.gpgx()->armRun(lim);
    e.backend()->resume();
    return e.waitPause(s);
}

} // namespace

TEST(run_to_return_stops_in_the_caller_with_the_stack_unwound)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K));                  // into the subroutine
    REQUIRE(e.backend()->getM68kRegs().pc == kSubEntry);
    const uint32_t entrySp = e.backend()->getM68kRegs().a[7];

    REQUIRE(runAndWait(e, RunLimits::toReturnFromHere(*e.gpgx())));
    CHECK(e.pauseReason() == StopReason::RunToReturn);
    const M68kRegs r = e.backend()->getM68kRegs();
    CHECK_EQ(r.pc, kAfterJsr);
    CHECK_EQ(r.a[7], entrySp + 4);
}

TEST(run_to_return_ignores_the_address_while_the_stack_is_deeper)
{
    // A routine that re-enters itself reaches its return address on the inner
    // return too; only the stack tells the two apart. Simulated by asking for a
    // return to kAfterJsr while claiming an entry SP below the real one.
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K));
    RunLimits lim;
    lim.toReturn = true;
    lim.returnAddr = kAfterJsr;
    lim.entrySp = e.backend()->getM68kRegs().a[7] + 0x100;   // "entered" higher up
    lim.maxInstructions = 50;
    REQUIRE(runAndWait(e, lim));
    CHECK(e.pauseReason() == StopReason::Budget);           // never matched
}

TEST(run_to_address_and_instruction_budget)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kJsr);
    RunLimits to;
    to.stopAddr = kSubRts;
    REQUIRE(runAndWait(e, to));
    CHECK(e.pauseReason() == StopReason::StopAddress);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubRts);

    e.parkM68kAt(kJsr);
    RunLimits budget;
    budget.maxInstructions = 2;                          // jsr, nop
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubRts);   // two retired, standing on the third
}

// After a stop delivered between frames (here: a halted CPU), the CPU stands
// on an instruction no hook has counted yet. The budget must still mean
// "instructions retired from here".
TEST(instruction_budget_after_a_between_frame_stop)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kHaltEntry);
    e.backend()->resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(e.pauseAndWait(1500));
    REQUIRE(e.pauseReason() == StopReason::Halted);

    e.parkM68kAt(kJsr);
    RunLimits budget;
    budget.maxInstructions = 2;                          // jsr, nop
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubRts);
}

TEST(frame_budget_stops_after_that_many_frames)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kJsr);
    const uint64_t f0 = e.host()->frameCount();
    RunLimits lim;
    lim.maxFrames = 3;
    REQUIRE(runAndWait(e, lim));
    CHECK(e.pauseReason() == StopReason::Budget);
    const uint64_t ran = e.host()->frameCount() - f0;
    CHECK(ran >= 3 && ran <= 4);
}
