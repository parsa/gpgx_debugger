// The opt-in access log: every matching memory access with the PC that made
// it, its address, size, direction and value. Bounded, filtered, off by
// default.

#include "test_harness.h"
#include "fixture.h"

using namespace synth;

namespace {

// Run the data routine (longword write then read of kDataAddr) and stop at
// its trailing bra.s, so both accesses have happened.
void runDataRoutine(t::Emu& e)
{
    Breakpoint park;
    park.type = BpType::PC; park.start = kDataRead + 6; park.end = kDataRead + 6;
    const int id = e.backend()->addBreakpoint(park);
    e.parkM68kAt(kDataEntry);
    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    e.waitPause(s);
    e.backend()->removeBreakpoint(id);
}

} // namespace

TEST(access_log_is_off_by_default)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    runDataRoutine(e);
    CHECK(!e.gpgx()->accessLogEnabled());
    CHECK(e.gpgx()->accessLog().entries.empty());
}

TEST(access_log_records_pc_address_size_direction_and_value)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    AccessLogConfig cfg;
    cfg.ranges.push_back({ AccessSpace::M68k, kDataAddr, kDataAddr + 3 });
    e.gpgx()->startAccessLog(cfg);
    runDataRoutine(e);
    const AccessLog log = e.gpgx()->accessLog();
    REQUIRE(log.entries.size() == 2);
    const AccessLogEntry& w = log.entries[0];
    CHECK_EQ(w.pc, kDataEntry);
    CHECK(w.access.write);
    CHECK(w.access.space == AccessSpace::M68k);
    CHECK_EQ(w.access.address, kDataAddr);
    CHECK_EQ(unsigned(w.access.size), 4u);
    CHECK_EQ(w.access.value, kDataValue);
    const AccessLogEntry& r = log.entries[1];
    CHECK_EQ(r.pc, kDataRead);
    CHECK(!r.access.write);
    CHECK_EQ(r.access.value, kDataValue);
    CHECK_EQ(log.dropped, 0u);
}

TEST(access_log_is_bounded_and_counts_what_it_dropped)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    AccessLogConfig cfg;
    cfg.capacity = 1;
    cfg.ranges.push_back({ AccessSpace::M68k, kDataAddr, kDataAddr + 3 });
    e.gpgx()->startAccessLog(cfg);
    runDataRoutine(e);
    const AccessLog log = e.gpgx()->accessLog();
    REQUIRE(log.entries.size() == 1);
    CHECK(log.entries[0].access.write);         // the first access is kept
    CHECK_EQ(log.dropped, 1u);
}

TEST(access_log_filter_excludes_other_addresses)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    AccessLogConfig cfg;
    cfg.ranges.push_back({ AccessSpace::M68k, 0xFF0000, 0xFF0003 });   // untouched
    e.gpgx()->startAccessLog(cfg);
    runDataRoutine(e);
    CHECK(e.gpgx()->accessLog().entries.empty());
    e.gpgx()->stopAccessLog();
    CHECK(!e.gpgx()->accessLogEnabled());
}
