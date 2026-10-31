#include "fixture.h"
#include <atomic>
#include <thread>

TEST(event_unsubscribe_disables_a_copied_pending_callback)
{
    t::Emu e;
    std::mutex mx;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::atomic<int> calls{0};
    const int blocker = e.host()->addEventSink([&](const DebugEvent& event) {
        if (event.type != DebugEvent::Type::Started) return;
        std::unique_lock<std::mutex> lock(mx);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    const int victim = e.host()->addEventSink([&](const DebugEvent& event) {
        if (event.type == DebugEvent::Type::Started) ++calls;
    });
    bool started = false;
    std::thread starter([&] { started = e.start(); });
    bool reached;
    {
        std::unique_lock<std::mutex> lock(mx);
        reached = cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    e.host()->removeEventSink(victim);
    { std::lock_guard<std::mutex> lock(mx); release = true; }
    cv.notify_all(); starter.join();
    e.host()->removeEventSink(blocker);
    REQUIRE(reached); REQUIRE(started);
    CHECK_EQ(calls.load(), 0);
}

TEST(event_unsubscribe_drains_an_inflight_callback)
{
    t::Emu e;
    std::mutex mx;
    std::condition_variable cv;
    bool entered = false, release = false, removing = false, removed = false;
    const int sink = e.host()->addEventSink([&](const DebugEvent& event) {
        if (event.type != DebugEvent::Type::Started) return;
        std::unique_lock<std::mutex> lock(mx);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    bool started = false;
    std::thread starter([&] { started = e.start(); });
    bool reached;
    {
        std::unique_lock<std::mutex> lock(mx);
        reached = cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    std::thread remover([&] {
        { std::lock_guard<std::mutex> lock(mx); removing = true; }
        cv.notify_all();
        e.host()->removeEventSink(sink);
        { std::lock_guard<std::mutex> lock(mx); removed = true; }
        cv.notify_all();
    });
    bool returned_early;
    {
        std::unique_lock<std::mutex> lock(mx);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return removing; });
        returned_early = cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return removed; });
        release = true;
    }
    cv.notify_all(); starter.join(); remover.join();
    REQUIRE(reached); REQUIRE(started);
    CHECK(!returned_early);
    CHECK(removed);
}
