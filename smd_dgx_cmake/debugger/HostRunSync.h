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

    // Emulation thread: a stop the host must learn about, or any other event.
    void pushStop(const Event& e)  { push(e, true); }
    void pushOther(const Event& e) { push(e, false); }

    // The host's poll. A stop handed over is the host's until it resumes.
    bool retrieve(Event* out, bool* more)
    {
        std::lock_guard<std::mutex> lk(mx_);
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
    bool releaseHold()
    {
        std::lock_guard<std::mutex> lk(mx_);
        const bool drive = !held_ || !movedOn_;
        held_ = movedOn_ = continueWanted_ = false;
        return drive;
    }

    // May the host act on the stop it believes in (e.g. step from it)?
    bool atHeldStop() const
    {
        std::lock_guard<std::mutex> lk(mx_);
        return !held_ || !movedOn_;
    }

    // A bridge client runs the machine. resumeBackend runs after the state
    // change, outside the lock; postContinue too, at most once per held stop.
    void run(const std::function<void()>& resumeBackend)
    {
        PostContinue post;
        {
            std::lock_guard<std::mutex> lk(mx_);
            for (auto it = q_.begin(); it != q_.end();)
                it = it->stop ? q_.erase(it) : it + 1;
            if (held_) {
                movedOn_ = true;
                if (!continueWanted_) { continueWanted_ = true; post = post_; }
            }
        }
        resumeBackend();
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
    struct Item { Event ev; bool stop; };
    void push(const Event& e, bool stop)
    {
        std::lock_guard<std::mutex> lk(mx_);
        q_.push_back(Item{e, stop});
    }

    mutable std::mutex mx_;
    std::deque<Item>   q_;
    PostContinue       post_;
    bool held_ = false;            // the host collected a stop and has not resumed
    bool movedOn_ = false;         // ...and a bridge client ran the machine since
    bool continueWanted_ = false;  // a continue was posted to the host for it
};
