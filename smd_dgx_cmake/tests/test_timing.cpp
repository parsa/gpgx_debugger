#include "fixture.h"
extern "C" {
unsigned int test_exception_cost(unsigned int);
unsigned int test_bus_refresh(int, unsigned int*);
unsigned int test_reset_refresh(unsigned int*);
unsigned int test_irq_phase(unsigned int, int, unsigned int*);
}

TEST(upstream_exception_costs_are_preserved)
{
    CHECK_EQ(test_exception_cost(6), 38u*7);
    CHECK_EQ(test_exception_cost(10), 34u*7);
    CHECK_EQ(test_exception_cost(11), 34u*7);
    for (unsigned int v = 25; v <= 31; ++v)
        CHECK_EQ(test_exception_cost(v), 44u*7);
}

TEST(upstream_bus_refresh_and_reset_sync_are_preserved)
{
    t::Emu e;
    REQUIRE(e.start()); REQUIRE(e.pauseAndWait());
    unsigned int due = 0, normal = 0, next_due = 0, next_normal = 0, reset = 0, next_reset = 0;
    e.backend()->runSafely([&] {
        due = test_bus_refresh(1, &next_due);
        normal = test_bus_refresh(0, &next_normal);
        reset = test_reset_refresh(&next_reset);
    });
    CHECK_EQ(due, 6u*7);
    CHECK_EQ(normal, 4u*7);
    CHECK_EQ(next_due, 128u*7);
    CHECK_EQ(next_normal, 128u*7);
    CHECK_EQ(next_reset, (reset / (128u*7) + 1) * (128u*7));
}

TEST(upstream_irq_phase_cost_and_wait_state_are_preserved)
{
    auto rom = synth::buildRom();
    synth::put32be(rom, 26*4, synth::kSubEntry);
    t::Emu e(rom);
    REQUIRE(e.start()); REQUIRE(e.pauseAndWait());
    const unsigned int costs[] = {50, 59, 58, 57, 56, 55, 54, 53, 52, 51};
    for (unsigned int phase = 0; phase < 10; ++phase) {
        unsigned int cycles = 0, stopped = 0;
        e.backend()->runSafely([&] { cycles = test_irq_phase(phase, 0, &stopped); });
        CHECK_EQ(cycles, costs[phase]*7);
    }
    unsigned int cycles = 0, stopped = 0;
    e.backend()->runSafely([&] { cycles = test_irq_phase(0, 1, &stopped); });
    CHECK_EQ(cycles, 0u);
    CHECK_EQ(stopped, 4u);
}
