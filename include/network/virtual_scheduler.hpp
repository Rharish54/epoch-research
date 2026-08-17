#pragma once

#include "core/types.hpp"

#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace chronos {

// Deterministic virtual-time event loop. Pending actions are ordered by
// (fire_time, insertion_sequence) -- never by wall clock, never by thread
// scheduling -- so a fixed input schedule always dispatches in one fixed
// order. Nothing here ever sleeps: the whole run completes in however long
// it takes to drain the heap, regardless of the simulated time span it
// covers (there could be a day of simulated time between two adjacent
// actions and this makes no difference to wall-clock runtime).
//
// Design note: boost::asio::io_context is the *executor* here, not the
// *clock*. The min-heap below is what actually orders events; io_context
// contributes a real handler-dispatch model (and satisfies this project's
// Boost.Asio requirement) rather than adding capability a bare loop
// couldn't provide. This is a deliberate, honest use of Asio for a
// simulation with no real I/O, not a workaround.
class VirtualTimeScheduler {
public:
    // Invoked with the simulated time at which it fires.
    using Action = std::function<void(NsTimestamp)>;

    // `io` must outlive this scheduler and must only ever be driven by this
    // scheduler's run() -- never call io.run()/poll() from elsewhere while a
    // VirtualTimeScheduler owns it, or dispatch order stops being a pure
    // function of (fire_time, seq).
    explicit VirtualTimeScheduler(boost::asio::io_context& io);

    // Enqueues `action` to fire at simulated time `fire_time`. Ties (equal
    // fire_time) are broken by insertion order -- the action scheduled
    // first fires first. Safe to call from inside a running action (the new
    // entry joins the same heap and is subject to the same ordering rules,
    // including firing before already-queued later actions if fire_time is
    // earlier than now()).
    void schedule_at(NsTimestamp fire_time, Action action);

    // Drains the queue in (fire_time, seq) order, dispatching each action
    // through the io_context. Returns the number of actions executed. Safe
    // to call again after actions scheduled a subsequent round of work.
    std::size_t run();

    // Simulated time of the action currently firing, or the last one that
    // fired. 0 if run() has never executed anything.
    NsTimestamp now() const noexcept { return now_; }
    std::size_t pending() const noexcept { return heap_.size(); }

private:
    struct Entry {
        NsTimestamp   fire_time;
        std::uint64_t seq;
        Action        action;
    };

    // Comparator for a max-heap over std::push_heap/pop_heap that produces
    // min-first popping order when negated by the heap's own convention:
    // std::push_heap/pop_heap maintain the heap property such that
    // comparator(a, b) == true means "a belongs later than b" -- so this
    // returns true when `a` should be considered "greater" (later), which
    // makes pop_heap surface the smallest (fire_time, seq) pair first.
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const noexcept {
            if (a.fire_time != b.fire_time) return a.fire_time > b.fire_time;
            return a.seq > b.seq;
        }
    };

    boost::asio::io_context& io_;
    std::vector<Entry>       heap_;
    std::uint64_t            next_seq_ = 0;
    NsTimestamp               now_ = 0;
};

} // namespace chronos
