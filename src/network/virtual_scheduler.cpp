#include "network/virtual_scheduler.hpp"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <utility>

namespace chronos {

VirtualTimeScheduler::VirtualTimeScheduler(boost::asio::io_context& io) : io_(io) {}

void VirtualTimeScheduler::schedule_at(NsTimestamp fire_time, Action action) {
    heap_.push_back(Entry{fire_time, next_seq_++, std::move(action)});
    std::push_heap(heap_.begin(), heap_.end(), Later{});
}

std::size_t VirtualTimeScheduler::run() {
    std::size_t executed = 0;

    while (!heap_.empty()) {
        std::pop_heap(heap_.begin(), heap_.end(), Later{});
        Entry entry = std::move(heap_.back()); // back() is non-const -- movable, unlike
                                                // std::priority_queue::top()
        heap_.pop_back();

        now_ = entry.fire_time;

        // Post through the io_context rather than calling entry.action
        // directly: this is what makes dispatch a real Asio handler queue
        // rather than a bare function call, satisfying this project's
        // Boost.Asio requirement honestly (see the class doc comment).
        // io_context latches "stopped" once run() returns for lack of work,
        // so it must be restarted before each one-handler run below --
        // omitting this silently no-ops every iteration after the first.
        boost::asio::post(io_, [action = std::move(entry.action), t = now_]() { action(t); });
        io_.restart();
        io_.run();
        ++executed;
    }

    return executed;
}

} // namespace chronos
