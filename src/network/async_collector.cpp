#include "network/async_collector.hpp"

#include <algorithm>
#include <cstdint>

namespace chronos {

AsyncCollector::AsyncCollector(std::span<MarketEvent> events) : events_(events) {}

void AsyncCollector::on_arrival(std::size_t event_index, NsTimestamp arrival_time) {
    MarketEvent& e = events_[event_index];
    e.receive_time = arrival_time;

    arrival_order_.push_back(event_index);
    const double latency = static_cast<double>(arrival_time - e.true_time);
    latency_ns_.push_back(latency);

    ++stats_.delivered;
    const auto latency_ns = static_cast<NsDuration>(latency);
    if (stats_.delivered == 1) {
        stats_.min_latency_ns = latency_ns;
        stats_.max_latency_ns = latency_ns;
    } else {
        stats_.min_latency_ns = std::min(stats_.min_latency_ns, latency_ns);
        stats_.max_latency_ns = std::max(stats_.max_latency_ns, latency_ns);
    }

    // Same-source inversion bookkeeping: a source's own events always
    // depart in strictly increasing true_time order (EventSource guarantees
    // this), so "departure order" for one source IS true_time order. If this
    // event's true_time is earlier than the latest true_time already
    // arrived from the same source, this event arrived out of that source's
    // departure order -- an inversion.
    const auto src = static_cast<std::size_t>(static_cast<std::uint32_t>(e.source_id));
    if (src >= source_seen_.size()) {
        source_seen_.resize(src + 1, false);
        last_arrived_departure_by_source_.resize(src + 1, 0);
    }
    if (source_seen_[src]) {
        ++stats_.same_source_pairs;
        if (e.true_time < last_arrived_departure_by_source_[src]) {
            ++stats_.same_source_inversions;
        }
    }
    source_seen_[src] = true;
    last_arrived_departure_by_source_[src] = std::max(last_arrived_departure_by_source_[src], e.true_time);
}

void schedule_source_delivery(VirtualTimeScheduler& scheduler, AsyncCollector& collector,
                               LatencyModel& latency, std::span<const MarketEvent> events,
                               std::size_t indices_begin, bool apply_reordering) {
    // Draw the whole slice's latencies up front, before any scheduling, so
    // RNG consumption order depends only on (source, event index) and never
    // on how the virtual-time heap happens to interleave arrivals.
    std::vector<NsDuration> delays;
    delays.reserve(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        delays.push_back(latency.sample_latency(LinkDirection::Inbound));
    }

    // Permutes the event<->latency pairing only -- draws strictly after
    // every latency sample above, from the same rng_, so it cannot alter
    // any latency value, only which event receives which one.
    if (apply_reordering) latency.apply_reordering(delays);

    for (std::size_t i = 0; i < events.size(); ++i) {
        const NsTimestamp arrival = events[i].true_time + delays[i];
        const std::size_t idx     = indices_begin + i;
        scheduler.schedule_at(arrival, [&collector, idx](NsTimestamp t) { collector.on_arrival(idx, t); });
    }
}

} // namespace chronos
