// Every stop says what caused it. Watchpoints also say which access did.

#include "test_harness.h"
#include "fixture.h"

using namespace synth;

namespace {

int watch(t::Emu& e, BpType type)
{
    Breakpoint bp;
    bp.type = type; bp.start = kDataAddr; bp.end = kDataAddr + 3;
    return e.backend()->addBreakpoint(bp);
}

} // namespace

TEST(write_watchpoint_stop_names_the_access)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const int id = watch(e, BpType::Write);
    e.parkM68kAt(kDataEntry);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    const DebugEvent p = e.pauseEvent();
    CHECK(p.reason == StopReason::Watchpoint);
    CHECK_EQ(p.bpId, id);
    CHECK(p.access.write);
    CHECK(p.access.space == AccessSpace::M68k);
    CHECK_EQ(p.access.address, kDataAddr);
    CHECK_EQ(unsigned(p.access.size), 4u);
    CHECK_EQ(p.access.value, kDataValue);
    CHECK_STR(describeStop(p),
              "reason=watchpoint access=write space=m68k addr=FF4276 size=4 value=12345678");
}

TEST(read_watchpoint_stop_names_the_access)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kDataEntry);
    const int id = watch(e, BpType::Read);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    const DebugEvent p = e.pauseEvent();
    CHECK(p.reason == StopReason::Watchpoint);
    CHECK_EQ(p.bpId, id);
    CHECK(!p.access.write);
    CHECK_EQ(p.access.address, kDataAddr);
    CHECK_EQ(p.access.value, kDataValue);       // what the read returned
}
