// A 68000 cannot fetch an instruction from an odd address: a jump, branch or
// return to one takes an address error (vector 3) from within that
// instruction. The frame and side effects below are those of a real MC68000 as
// established with WinUAE's hardware-verified 68000 core (gencpu.cpp,
// newcpu.cpp: exception3_read_prefetch*): stacked PC, access address = the odd
// target, IR = the branching opcode, status = read, instruction, program space.

#include "test_harness.h"
#include "fixture.h"

#include <chrono>
#include <cstring>
#include <thread>

using namespace synth;

namespace {

constexpr uint32_t P      = 0xFF0100;     // code under test, in RAM
constexpr uint32_t kSsp   = 0xFFF000;
constexpr uint64_t kAddrErrorBit = uint64_t(1) << 3;

struct Frame {
    bool     faulted = false;
    uint32_t excPc = 0, framePc = 0, address = 0;
    ExcAccess access = ExcAccess::None;
    uint16_t status = 0, ir = 0, sr = 0;
    uint32_t stackedPc = 0;
    uint32_t spBefore = 0;                // A7 before the address error frame
    M68kRegs regs{};                      // registers when stopped on the fault
};

uint16_t be16(const std::vector<uint8_t>& b, size_t o) { return uint16_t((b[o] << 8) | b[o + 1]); }
uint32_t be32(const std::vector<uint8_t>& b, size_t o) { return (uint32_t(be16(b, o)) << 16) | be16(b, o + 2); }

// Put `code` at P, set up registers (supervisor, A7 = kSsp unless changed),
// run, and stop on an address error if one is taken.
template <class Setup>
Frame run(t::Emu& e, std::vector<uint8_t> code, Setup setup)
{
    // Anything after the code under test parks (bra.s *): a run that does not
    // fault ends there.
    code.push_back(0x60); code.push_back(0xFE);
    CHECK(e.backend()->writeMemory(P, code.data(), uint32_t(code.size())));
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = P;
    r.sr = 0x2700;
    r.a[7] = kSsp;
    r.isp = kSsp;
    setup(r);
    e.backend()->setM68kRegs(r);
    e.gpgx()->setExceptionCatch(kAddrErrorBit);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    Frame f;
    if (e.waitPause(s, 300) && e.pauseReason() == StopReason::Exception) {
        const DebugEvent p = e.pauseEvent();
        f.faulted = p.exc.vector == 3;
        f.excPc = p.exc.pc;
        f.framePc = p.exc.framePc;
        f.address = p.exc.address;
        f.access = p.exc.access;
        f.regs = e.backend()->getM68kRegs();
        const auto fr = e.backend()->readMemory(f.regs.a[7], 14);
        f.status = be16(fr, 0);
        f.ir = be16(fr, 6);
        f.sr = be16(fr, 8);
        f.stackedPc = be32(fr, 10);
        f.spBefore = f.regs.a[7] + 14;
    } else {
        e.pauseAndWait();
        f.regs = e.backend()->getM68kRegs();
    }
    e.gpgx()->setExceptionCatch(0);
    return f;
}
auto none = [](M68kRegs&) {};

// The fields every odd-target fault shares.
void checkFetchFault(const Frame& f, uint16_t opcode, uint32_t target, uint32_t stackedPc, uint16_t fc = 6)
{
    CHECK(f.faulted);
    CHECK_EQ(f.excPc, P);                           // raised by the branching instruction
    CHECK_EQ(f.address, target);
    CHECK(f.access == ExcAccess::Fetch);
    CHECK_EQ(unsigned(f.ir), unsigned(opcode));
    CHECK_EQ(f.stackedPc, stackedPc);
    CHECK_EQ(f.framePc, stackedPc);
    CHECK_EQ(unsigned(f.status & 0x1F), unsigned(0x10 | fc));   // read, instruction (I/N=0), program
}

} // namespace

TEST(jmp_to_an_odd_address_is_an_address_error)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    // jmp (a0)
    Frame f = run(e, {0x4E, 0xD0}, [](M68kRegs& r) { r.a[0] = 0xFF0201; });
    checkFetchFault(f, 0x4ED0, 0xFF0201, P + 2);
    CHECK_EQ(f.spBefore, kSsp);
    CHECK_EQ(unsigned(f.sr), 0x2700u);
    // jmp $00FF0201.l
    f = run(e, {0x4E, 0xF9, 0x00, 0xFF, 0x02, 0x01}, none);
    checkFetchFault(f, 0x4EF9, 0xFF0201, P + 2);
    // An even target still jumps.
    f = run(e, {0x4E, 0xD0}, [](M68kRegs& r) { r.a[0] = P + 2; });   // onto the parking bra.s
    CHECK(!f.faulted);
    CHECK_EQ(f.regs.pc, P + 2);
}

TEST(jsr_to_an_odd_address_faults_before_pushing)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    Frame f = run(e, {0x4E, 0x90}, [](M68kRegs& r) { r.a[0] = 0xFF0201; });   // jsr (a0)
    checkFetchFault(f, 0x4E90, 0xFF0201, P + 2);
    CHECK_EQ(f.spBefore, kSsp);                                            // nothing pushed
    f = run(e, {0x4E, 0xA8, 0x00, 0x01}, [](M68kRegs& r) { r.a[0] = 0xFF0200; });   // jsr 1(a0)
    checkFetchFault(f, 0x4EA8, 0xFF0201, P + 2);
    f = run(e, {0x4E, 0xB8, 0x02, 0x01}, none);                           // jsr $0201.w
    checkFetchFault(f, 0x4EB8, 0x000201, P + 4);
    f = run(e, {0x4E, 0xB9, 0x00, 0xFF, 0x02, 0x01}, none);               // jsr $00FF0201.l
    checkFetchFault(f, 0x4EB9, 0xFF0201, P + 6);
    CHECK_EQ(f.spBefore, kSsp);
}

TEST(returns_to_an_odd_address_fault_after_popping)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    const uint8_t retPc[] = {0x00, 0xFF, 0x02, 0x01};
    // rts
    REQUIRE(e.backend()->writeMemory(kSsp, retPc, 4));
    Frame f = run(e, {0x4E, 0x75}, none);
    checkFetchFault(f, 0x4E75, 0xFF0201, P + 2);
    CHECK_EQ(f.spBefore, kSsp + 4);
    // rtr: the CCR is restored first
    const uint8_t rtrFrame[] = {0x00, 0x1F, 0x00, 0xFF, 0x02, 0x01};
    REQUIRE(e.backend()->writeMemory(kSsp, rtrFrame, 6));
    f = run(e, {0x4E, 0x77}, none);
    checkFetchFault(f, 0x4E77, 0xFF0201, P + 2);
    CHECK_EQ(f.spBefore, kSsp + 6);
    CHECK_EQ(unsigned(f.sr), 0x271Fu);
    // rte: the whole SR is restored first
    const uint8_t rteFrame[] = {0x27, 0x04, 0x00, 0xFF, 0x02, 0x01};
    REQUIRE(e.backend()->writeMemory(kSsp, rteFrame, 6));
    f = run(e, {0x4E, 0x73}, none);
    checkFetchFault(f, 0x4E73, 0xFF0201, P + 2);
    CHECK_EQ(f.spBefore, kSsp + 6);
    CHECK_EQ(unsigned(f.sr), 0x2704u);
    // rte to user mode: the fault is a user program fetch, taken on the supervisor stack
    const uint8_t rteUser[] = {0x00, 0x00, 0x00, 0xFF, 0x02, 0x01};
    REQUIRE(e.backend()->writeMemory(kSsp, rteUser, 6));
    f = run(e, {0x4E, 0x73}, [](M68kRegs& r) { r.usp = 0xFFE000; });
    checkFetchFault(f, 0x4E73, 0xFF0201, P + 2, 2);
    CHECK_EQ(f.spBefore, kSsp + 6);
    CHECK_EQ(unsigned(f.sr), 0x0000u);
    CHECK_EQ(f.regs.usp, 0xFFE000u);
}

TEST(branches_to_an_odd_address_fault)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    // bra.s *+3: the stacked PC is the branch's + 2
    Frame f = run(e, {0x60, 0x01}, none);
    checkFetchFault(f, 0x6001, P + 3, P + 2);
    f = run(e, {0x60, 0x00, 0x00, 0x01}, none);                // bra.w *+3
    checkFetchFault(f, 0x6000, P + 3, P + 2);
    f = run(e, {0x60, 0xFF}, none);                            // bra with an 8-bit -1 (68000)
    checkFetchFault(f, 0x60FF, P + 1, P + 2);
    // A conditional branch not taken does not fetch its target.
    f = run(e, {0x67, 0x01}, [](M68kRegs& r) { r.sr = 0x2700; });   // beq.s, Z clear
    CHECK(!f.faulted);
    CHECK_EQ(f.regs.pc, P + 2);
    f = run(e, {0x67, 0x01}, [](M68kRegs& r) { r.sr = 0x2704; });   // beq.s, Z set
    checkFetchFault(f, 0x6701, P + 3, P + 2);
    CHECK_EQ(unsigned(f.sr), 0x2704u);
}

TEST(bsr_to_an_odd_address_pushes_then_faults_with_the_target_stacked)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    Frame f = run(e, {0x61, 0x01}, none);                      // bsr.s *+3
    checkFetchFault(f, 0x6101, P + 3, P + 3);
    CHECK_EQ(f.spBefore, kSsp - 4);
    CHECK_EQ(be32(e.backend()->readMemory(kSsp - 4, 4), 0), P + 2);   // return address pushed
    f = run(e, {0x61, 0x00, 0x00, 0x01}, none);                // bsr.w *+3
    checkFetchFault(f, 0x6100, P + 3, P + 3);
    CHECK_EQ(be32(e.backend()->readMemory(kSsp - 4, 4), 0), P + 4);
}

TEST(dbcc_to_an_odd_address_faults_before_counting)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    // dbf d0,*+3: the 68000 fetches the target before it counts, so it faults
    // with the counter untouched, even when the counter is about to expire.
    Frame f = run(e, {0x51, 0xC8, 0x00, 0x01}, [](M68kRegs& r) { r.d[0] = 5; });
    checkFetchFault(f, 0x51C8, P + 3, P + 5);
    CHECK_EQ(f.regs.d[0], 5u);
    f = run(e, {0x51, 0xC8, 0x00, 0x01}, [](M68kRegs& r) { r.d[0] = 0; });
    checkFetchFault(f, 0x51C8, P + 3, P + 5);
    CHECK_EQ(f.regs.d[0], 0u);
    // A true condition ends the loop without fetching the target.
    f = run(e, {0x57, 0xC8, 0x00, 0x01}, [](M68kRegs& r) { r.sr = 0x2704; r.d[0] = 5; });   // dbeq, Z set
    CHECK(!f.faulted);
    CHECK_EQ(f.regs.pc, P + 4);
    CHECK_EQ(f.regs.d[0], 5u);
}

TEST(an_odd_exception_vector_is_an_address_error)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    // trap #2 (vector 34) through an odd handler address: the trap's frame is
    // stacked, then the address error's, with the vector's own address as PC.
    const auto saved = e.backend()->readMemory(34 * 4, 4);
    const uint8_t odd[] = {0x00, 0x00, 0x06, 0x01};
    REQUIRE(e.backend()->writeMemory(34 * 4, odd, 4));
    Frame f = run(e, {0x4E, 0x42}, none);
    CHECK(f.faulted);
    CHECK_EQ(f.address, 0x000601u);
    CHECK(f.access == ExcAccess::Fetch);
    CHECK_EQ(unsigned(f.ir), 0x4E42u);
    CHECK_EQ(f.stackedPc, 34u * 4);
    CHECK_EQ(f.spBefore, kSsp - 6);                           // the trap's own frame below it
    CHECK_EQ(unsigned(f.status & 0x1F), 0x16u);
    REQUIRE(e.backend()->writeMemory(34 * 4, saved.data(), 4));
}

namespace {
// move.w #$81xx,$C00004.l: VDP register 1 (0x64: display, VINT on; 0x44: VINT off)
std::vector<uint8_t> vdpReg1(uint8_t v) { return {0x33, 0xFC, 0x81, v, 0x00, 0xC0, 0x00, 0x04}; }
// Let frames run with VINT off (the synthetic ROM never enables it): a VBlank
// is then pending, and enabling VINT raises level 6.
bool vblankPending(t::Emu& e)
{
    e.backend()->resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    return e.pauseAndWait();
}
}

TEST(an_odd_interrupt_vector_is_an_address_error)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(vblankPending(e));
    // Enabling VINT then raises level 6 (autovector 30) through an odd handler.
    const auto saved = e.backend()->readMemory(30 * 4, 4);
    const uint8_t odd[] = {0x00, 0x00, 0x06, 0x01};
    REQUIRE(e.backend()->writeMemory(30 * 4, odd, 4));
    Frame f = run(e, vdpReg1(0x64), [](M68kRegs& r) { r.sr = 0x2000; });
    CHECK(f.faulted);
    CHECK_EQ(f.address, 0x000601u);
    CHECK_EQ(f.stackedPc, 30u * 4);                  // the vector's own address
    CHECK_EQ(unsigned(f.ir), 30u);                   // an interrupt stacks its vector number
    CHECK_EQ(unsigned(f.status & 0x1F), 0x1Eu);      // read, not an instruction (I/N=1), program
    CHECK_EQ(unsigned(f.sr & 0xFF00), 0x2600u);      // interrupt mask already raised
    CHECK_EQ(f.spBefore, kSsp - 6);                  // the interrupt's own frame below it
    REQUIRE(e.backend()->writeMemory(30 * 4, saved.data(), 4));
}

// The instruction run between enabling a pending interrupt and raising its
// level (the one-instruction IRQ latency m68k_set_irq_delay emulates) can
// itself fault: the level must still be raised, and later interrupts through
// that path must still come one instruction after they are enabled.
TEST(a_fault_in_the_irq_latency_instruction_keeps_interrupts_working)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(vblankPending(e));
    auto code = vdpReg1(0x64);
    code.insert(code.end(), {0x4E, 0xD0});           // jmp (a0): the delayed instruction, odd target
    Frame f = run(e, code, [](M68kRegs& r) { r.sr = 0x2700; r.a[0] = 0xFF0201; });
    CHECK(f.faulted);
    CHECK_EQ(f.stackedPc, P + 10);
    // Interrupts still arrive through the latency path: VINT off, then on with
    // interrupts unmasked must take VBlank (autovector 30 -> the ROM's handler).
    auto again = vdpReg1(0x44);                      // level back to 0
    again.insert(again.end(), {0x46, 0xFC, 0x20, 0x00});   // move #$2000,sr: unmask
    const auto on = vdpReg1(0x64);                   // pending VBlank: raised after one instruction
    again.insert(again.end(), on.begin(), on.end());
    again.insert(again.end(), {0x52, 0x81, 0x60, 0xFC});   // addq.l #1,d1; bra.s -4
    REQUIRE(e.backend()->writeMemory(P + 0x40, again.data(), uint32_t(again.size())));
    M68kRegs r = e.backend()->getM68kRegs();
    r.pc = P + 0x40;
    r.sr = 0x2700;                  // the code unmasks once the level is back to 0
    r.a[7] = kSsp;
    r.d[1] = 0;
    e.backend()->setM68kRegs(r);
    e.backend()->resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(e.pauseAndWait());
    const M68kRegs after = e.backend()->getM68kRegs();
    if (!(after.pc >= kExcHandler && after.pc < kExcHandler + 4))
        std::printf("    pc=%06X sr=%04X\n", after.pc, after.sr);
    CHECK(after.pc >= kExcHandler && after.pc < kExcHandler + 4);   // in the VBlank handler
    CHECK_EQ(unsigned(after.sr & 0x0700), 0x0600u);
    // ...taken one instruction after enabling it (the latency path), not at
    // some later VBlank: the counting loop ran exactly once.
    CHECK_EQ(after.d[1], 1u);
    e.gpgx()->setExceptionCatch(0);
}
