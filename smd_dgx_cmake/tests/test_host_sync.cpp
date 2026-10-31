// HostRunSync: a host's (IDA's) view of the run state, kept consistent with
// the emulator without the emulator side ever waiting for the host.
#include "test_harness.h"

#include "debugger/HostRunSync.h"

#include <functional>
#include <vector>

namespace {
// Events are ints here: positive = a stop with that number, negative = other.
struct Model {
    HostRunSync<int> sync;
    int posted = 0;
    uint64_t epoch = 1;
    Model()
    {
        sync.setPostContinue([this] { ++posted; });
        sync.setEpochSource([this] { return epoch; });
    }
    void run(const std::function<void()>& resume) { sync.run([this, resume] { ++epoch; resume(); }, {}); }
    void stop(int n) { sync.pushStop(n, epoch); }
    std::vector<int> drain()
    {
        std::vector<int> out;
        int e; bool more = true;
        while (sync.retrieve(&e, &more)) out.push_back(e);
        return out;
    }
};
}

TEST(host_sync_drops_a_stop_the_host_never_collected_once_the_machine_runs)
{
    Model m;
    m.stop(1);
    int resumed = 0;
    m.run([&] { ++resumed; });
    CHECK_EQ(resumed, 1);
    CHECK_EQ(m.posted, 0);                  // nothing held: nothing to catch up
    m.stop(2);
    const auto got = m.drain();
    REQUIRE(got.size() == size_t(1));
    CHECK_EQ(got[0], 2);                    // the fresh stop only, never the stale one
}

TEST(host_sync_keeps_events_that_are_not_stops)
{
    Model m;
    m.sync.pushOther(-1);
    m.stop(1);
    m.sync.pushOther(-2);
    m.run([] {});
    const auto got = m.drain();
    REQUIRE(got.size() == size_t(2));
    CHECK_EQ(got[0], -1);
    CHECK_EQ(got[1], -2);
}

TEST(host_sync_a_held_stop_gets_one_continue_and_its_resume_leaves_the_machine_alone)
{
    Model m;
    m.stop(1);
    REQUIRE(m.drain().size() == size_t(1));     // the host suspends at stop 1
    CHECK(m.sync.atHeldStop());
    int resumed = 0;
    m.run([&] { ++resumed; });             // a bridge client runs on
    m.run([&] { ++resumed; });             // ...twice
    CHECK_EQ(resumed, 2);                       // the machine ran at once, both times
    CHECK_EQ(m.posted, 1);                      // one continue asked of the host
    CHECK(m.sync.continueWanted());
    CHECK(!m.sync.atHeldStop());                // nothing left to step from
    m.stop(2);                         // the machine stops again meanwhile
    // The host gets to the continue: its resume must not run the machine on
    // from stop 2, which it has not even seen.
    CHECK(!m.sync.releaseHold());
    CHECK(!m.sync.continueWanted());
    const auto got = m.drain();
    REQUIRE(got.size() == size_t(1));
    CHECK_EQ(got[0], 2);                        // it suspends at the fresh stop
}

TEST(host_sync_the_hosts_own_resume_drives_the_machine)
{
    Model m;
    m.stop(1);
    REQUIRE(m.drain().size() == size_t(1));
    CHECK(m.sync.releaseHold());                // nobody moved it: the host runs it
    CHECK_EQ(m.posted, 0);
    CHECK(m.sync.releaseHold());                // not holding anything: drive as before
}

TEST(host_sync_clear_forgets_everything)
{
    Model m;
    m.stop(1);
    m.drain();
    m.run([] {});
    m.sync.clear();
    CHECK(!m.sync.continueWanted());
    CHECK(m.sync.atHeldStop());
    CHECK(m.drain().empty());
}

TEST(host_sync_never_hands_over_a_stop_published_after_the_machine_ran_on)
{
    Model m;
    const uint64_t stoppedIn = m.epoch;         // the machine stops in this run...
    int resumed = 0;
    m.run([&] { ++resumed; });                  // ...a bridge client runs it on...
    m.sync.pushStop(7, stoppedIn);              // ...and only then is the stop published
    CHECK(m.drain().empty());                   // the host never suspends on it
    m.stop(8);                                  // a stop of the current run is handed over
    const auto got = m.drain();
    REQUIRE(got.size() == size_t(1));
    CHECK_EQ(got[0], 8);
}
