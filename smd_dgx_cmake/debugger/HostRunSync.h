#pragma once
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

// Keeps a host debugger's view of the run state (IDA's) consistent with the
// emulator, without ever making the emulator side wait for the host.
//
// The host learns about stops only by polling events (IDA: ev_get_debug_event),
// and it can only be made to continue on its own main thread. That thread is
// often busy: a headless script pumping wait_for_next_event, a long analysis,
// a modal dialog. Bridge clients drive the machine meanwhile, so waiting for
// the host to agree before every resume stalls them for as long as the host is
// busy — and giving up and resuming anyway leaves the host suspended at a stop
// the emulator has long left, which is how IDA and the emulator drifted apart.
//
// So the emulator side never waits; the host catches up:
//   - A stop the host has not collected yet goes stale the moment the machine
//     runs again: run() drops it, so the host never suspends on it.
//   - A stop the host HAS collected is the host's: it is suspended there. When a
//     bridge client runs the machine anyway, run() asks the host to continue
//     (postContinue, which must not wait) and the host's resume then finds the
//     machine already gone (releaseHold() == false) and leaves it alone. Any
//     newer stop is queued, so the host suspends there next — fresh, never stale.
//
// Event is the host's event type. Call nothing that can block while holding
// the internal lock: every callback runs outside it.
template <class Event>
class HostRunSync {
public:
    using PostContinue = std::function<void()>;

    void setPostContinue(PostContinue p)
    {
        std::lock_guard<std::mutex> lk(mx_);
        post_ = std::move(p);
    }
    // The emulator's current run epoch (GpgxBackend::runEpoch). A stop is
    // published by a sink that may run after the machine is already marked
    // paused, so a bridge client can run the machine on before the stop is
    // queued here; its epoch is then out of date and the host never sees it.
    void setEpochSource(std::function<uint64_t()> f)
    {
        std::lock_guard<std::mutex> lk(mx_);
        epochNow_ = std::move(f);
    }

    // Emulation thread: a stop the host must learn about (with the run epoch
    // it ended), or any other event.
    void pushStop(const Event& e, uint64_t epoch = 0) { push(e, true, epoch); }
    void pushOther(const Event& e) { push(e, false, 0); }

    // The host's poll. A stop handed over is the host's until it resumes.
    bool retrieve(Event* out, bool* more)
    {
        std::lock_guard<std::mutex> lk(mx_);
        while (!q_.empty() && q_.front().stop && epochNow_ && q_.front().epoch != epochNow_())
            q_.pop_front();                     // run past before it was queued
        if (q_.empty()) return false;
        Item it = std::move(q_.front());
        q_.pop_front();
        if (it.stop) { held_ = true; movedOn_ = false; }
        *out = std::move(it.ev);
        *more = !q_.empty();
        return true;
    }

    // The host resumed (continue, step, thread continue). True when the machine
    // still stands where the host stopped, so the host's resume should drive
    // it; false when a bridge client already ran it on.
    //
    // To drive it, pass beginRun: the new run epoch is taken under the lock, so
    // a bridge run cannot slip in between this decision and the host's resume
    // unnoticed; resume with GpgxBackend::resumeIfEpoch(*epoch), which declines
    // if a bridge client ran the machine (to a newer stop) in the meantime.
    bool releaseHold(const std::function<uint64_t()>& beginRun = {}, uint64_t* epoch = nullptr)
    {
        std::lock_guard<std::mutex> lk(mx_);
        const bool drive = !held_ || !movedOn_;
        held_ = movedOn_ = continueWanted_ = false;
        if (drive && beginRun && epoch) *epoch = beginRun();
        return drive;
    }

    // May the host act on the stop it believes in (e.g. step from it)?
    bool atHeldStop() const
    {
        std::lock_guard<std::mutex> lk(mx_);
        return !held_ || !movedOn_;
    }

    // A bridge client runs the machine. The transition to running (a new run
    // epoch, the machine unpaused: GpgxBackend::startRunning) happens under
    // the lock, so the host cannot collect a stop between deciding what it
    // holds and the machine actually running; a stop collected after it is of
    // this run. notify (announcing the resume; it reaches event sinks) and
    // postContinue run after, outside the lock.
    void run(const std::function<void()>& transition, const std::function<void()>& notify)
    {
        PostContinue post;
        {
            std::lock_guard<std::mutex> lk(mx_);
            for (auto it = q_.begin(); it != q_.end();)
                it = it->stop ? q_.erase(it) : it + 1;
            if (transition) transition();
            if (held_) {
                movedOn_ = true;
                if (!continueWanted_) { continueWanted_ = true; post = post_; }
            }
        }
        if (notify) notify();
        if (post) post();
    }

    // Checked by the posted continue when the host gets to it.
    bool continueWanted() const
    {
        std::lock_guard<std::mutex> lk(mx_);
        return continueWanted_ && held_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mx_);
        q_.clear();
        held_ = movedOn_ = continueWanted_ = false;
    }

private:
    struct Item { Event ev; bool stop; uint64_t epoch; };
    void push(const Event& e, bool stop, uint64_t epoch)
    {
        std::lock_guard<std::mutex> lk(mx_);
        q_.push_back(Item{e, stop, epoch});
    }

    mutable std::mutex mx_;
    std::deque<Item>   q_;
    PostContinue       post_;
    std::function<uint64_t()> epochNow_;
    bool held_ = false;            // the host collected a stop and has not resumed
    bool movedOn_ = false;         // ...and a bridge client ran the machine since
    bool continueWanted_ = false;  // a continue was posted to the host for it
};
