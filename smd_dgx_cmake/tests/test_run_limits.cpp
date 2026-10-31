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

// Stop the 68000 a few instructions into a frame: the Z80 runs after the
// 68000 in each line, so it now lags, and a 68000 write that synchronises the
// Z80 (BUSREQ/RESET) runs Z80 instructions inside that write.
bool lagZ80BehindM68k(t::Emu& e)
{
    const uint64_t f = e.pauseSeq();
    e.gpgx()->requestFramePause();
    e.backend()->resume();
    if (!e.waitPause(f) || !e.gpgx()->atFrameBoundary()) return false;
    RunLimits few;
    few.maxInstructions = 20;
    return runAndWait(e, few) && e.pauseReason() == StopReason::Budget;
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

// The same after recovering from a caught exception by moving PC.
TEST(instruction_budget_after_moving_pc_on_an_exception_stop)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.gpgx()->setExceptionCatch(uint64_t(1) << 4);
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = kIllegalEntry; r.sr = 0x2700; r.a[7] = kInitSp;
    e.backend()->setM68kRegs(r);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    REQUIRE(e.pauseReason() == StopReason::Exception);
    e.gpgx()->setExceptionCatch(0);

    e.parkM68kAt(kJsr);
    RunLimits budget;
    budget.maxInstructions = 2;                          // jsr, nop
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubRts);
}

// A pause delivered between frames must not let the frame that just ended
// count against a frame budget armed during it.
// Stopped on the Z80, the 68000 is between instructions: its next hook has
// not run. The 68000 budget still counts instructions retired from here.
TEST(instruction_budget_after_a_z80_stop)
{
    t::Emu e;
    REQUIRE(e.start());
    e.backend()->resume();                               // boot releases the Z80
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(e.pauseAndWait());
    REQUIRE(e.stepAndWait(Cpu::Z80));
    REQUIRE(e.pauseCpu() == Cpu::Z80);

    e.parkM68kAt(kJsr);
    RunLimits budget;
    budget.maxInstructions = 1;                          // the jsr
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubEntry);
}

// A 68000 write to the Z80 RESET/BUSREQ registers first runs the Z80 up to the
// 68000's time, inside that write. A Z80 stop there is inside a 68000
// instruction, which still retires after the stop.
TEST(instruction_budget_after_a_z80_stop_inside_a_68000_write)
{
    t::Emu e;
    REQUIRE(e.start());
    e.backend()->resume();                               // boot releases the Z80
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(e.pauseAndWait());
    REQUIRE(lagZ80BehindM68k(e));

    Breakpoint zbp;
    zbp.type = BpType::PC; zbp.cpu = Cpu::Z80; zbp.start = 0; zbp.end = 0xFFFF;   // anywhere
    const int id = e.backend()->addBreakpoint(zbp);
    e.parkM68kAt(kZ80BusWait + 8);                       // move.w #$100,$A11100 syncs the Z80
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    e.backend()->removeBreakpoint(id);
    REQUIRE(e.pauseCpu() == Cpu::Z80);
    CHECK(e.gpgx()->stopInsideM68kInstruction());

    RunLimits budget;
    budget.maxInstructions = 1;                          // the interrupted write
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kZ80BusWait + 0x10);
}

// A Z80 stop in the Z80's own time slice is between 68000 instructions.
TEST(z80_stop_in_its_own_time_slice_is_not_inside_a_68000_instruction)
{
    t::Emu e;
    REQUIRE(e.start());
    e.backend()->resume();                               // boot releases the Z80
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(e.pauseAndWait());
    e.parkM68kAt(kJsr);                                  // a loop that never touches the Z80
    REQUIRE(e.stepAndWait(Cpu::Z80));
    REQUIRE(e.pauseCpu() == Cpu::Z80);
    CHECK(!e.gpgx()->stopInsideM68kInstruction());
}

// A 68000-to-VRAM DMA runs line by line from the scheduler, between 68000
// instructions.
// A VDP watchpoint hit by it is not inside a 68000 instruction, and a budget
// armed there counts the next instruction as unretired.
TEST(vdp_watchpoint_hit_by_dma_is_between_68000_instructions)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    // In work RAM: a 0x1000-word DMA from $FF0000 to VRAM 0 (the command
    // write starts it), then loop nop; bra.s back to the nop.
    const uint16_t ctrl[] = { 0x8114, 0x8F02, 0x9300, 0x9410, 0x9500, 0x9680, 0x977F, 0x4000, 0x0080 };
    std::vector<uint8_t> code;
    auto w16 = [&](uint16_t v) { code.push_back(uint8_t(v >> 8)); code.push_back(uint8_t(v)); };
    for (uint16_t c : ctrl) { w16(0x33FC); w16(c); w16(0x00C0); w16(0x0004); }  // move.w #c,$C00004
    const uint32_t loop = 0xFF0300 + uint32_t(code.size());
    w16(0x4E71); w16(0x60FC);                                                   // nop; bra.s nop
    e.backend()->writeMemory(0xFF0300, code.data(), uint32_t(code.size()));

    Breakpoint watch;
    watch.type = BpType::Write; watch.is_vdp = true; watch.start = 0x1F00; watch.end = 0x1F00;
    const int id = e.backend()->addBreakpoint(watch);
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = 0xFF0300; r.sr = 0x2700; r.a[7] = kInitSp;
    e.backend()->setM68kRegs(r);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    e.backend()->removeBreakpoint(id);
    REQUIRE(e.pauseReason() == StopReason::Watchpoint);
    REQUIRE(e.pauseEvent().access.space == AccessSpace::Vdp);
    CHECK(!e.gpgx()->stopInsideM68kInstruction());

    const uint32_t standing = e.backend()->getM68kRegs().pc;     // the next 68000 instruction
    REQUIRE(standing == loop || standing == loop + 2);
    RunLimits budget;
    budget.maxInstructions = 1;
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, standing == loop ? loop + 2 : loop);
}

// An address error unwinds its instruction before stacking the frame: a
// watchpoint on that frame is not inside an instruction that will retire.
TEST(instruction_budget_after_a_watchpoint_on_an_address_error_frame)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    Breakpoint watch;
    watch.type = BpType::Write; watch.start = kInitSp - 4; watch.end = kInitSp - 1;   // stacked PC
    const int id = e.backend()->addBreakpoint(watch);
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = kAddrErrEntry; r.sr = 0x2700; r.a[7] = kInitSp;
    e.backend()->setM68kRegs(r);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    e.backend()->removeBreakpoint(id);
    REQUIRE(e.pauseReason() == StopReason::Watchpoint);
    CHECK(e.gpgx()->stopInsideM68kInstruction());        // the exception is unfinished

    RunLimits budget;
    budget.maxInstructions = 1;                          // the handler's nop
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kExcHandler + 2);
}

// And when the stop on a caught exception is simply continued: resuming
// finishes the exception, so the first instruction retired is the handler's.
TEST(instruction_budget_after_continuing_an_exception_stop)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.gpgx()->setExceptionCatch(uint64_t(1) << 4);
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = kIllegalEntry; r.sr = 0x2700; r.a[7] = kInitSp;
    e.backend()->setM68kRegs(r);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    REQUIRE(e.pauseReason() == StopReason::Exception);
    e.gpgx()->setExceptionCatch(0);

    RunLimits budget;
    budget.maxInstructions = 1;                          // the handler's nop
    REQUIRE(runAndWait(e, budget));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK_EQ(e.backend()->getM68kRegs().pc, kExcHandler + 2);
}

TEST(frame_budget_after_a_between_frame_stop_runs_a_whole_frame)
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
    const uint64_t f0 = e.host()->frameCount();
    RunLimits lim;
    lim.maxFrames = 1;
    REQUIRE(runAndWait(e, lim));
    CHECK(e.pauseReason() == StopReason::Budget);
    CHECK(e.host()->frameCount() - f0 >= 1);             // a whole frame ran
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
