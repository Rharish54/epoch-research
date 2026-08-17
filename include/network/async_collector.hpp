#pragma once

#include "core/types.hpp"
#include "network/latency_model.hpp"
#include "network/virtual_scheduler.hpp"
#include "simulation/market_event.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace chronos {

struct DeliveryStats {
    std::size_t delivered              = 0;
    // Adjacent-in-departure-order pairs from the SAME source that arrived
    // out of that order -- i.e. the network reordered one source's own
    // stream. Not a measure of cross-source interleaving (which is
    // expected and not a "violation" of anything).
    std::size_t same_source_pairs      = 0;
    std::size_t same_source_inversions = 0;
    NsDuration  min_latency_ns         = 0;
    NsDuration  max_latency_ns         = 0;
};

// Receives simulated event arrivals in virtual-time order and stamps
// receive_time on the borrowed events in place. Single-threaded by
// construction: VirtualTimeScheduler::run() dispatches one handler at a
// time, so no synchronization is needed or wanted here (see CLAUDE.md: no
// premature multithreading).
//
// Borrows `events` via std::span rather than owning a copy: `events` must
// outlive this collector and must not be reallocated (e.g. via push_back)
// between construction and the point the scheduler finishes running --
// otherwise the span dangles. Callers should reserve() the backing vector
// up front and stop appending to it before constructing the collector.
class AsyncCollector {
public:
    explicit AsyncCollector(std::span<MarketEvent> events);

    // Called by the scheduler at the arrival's simulated time. `event_index`
    // indexes into the span passed to the constructor.
    void on_arrival(std::size_t event_index, NsTimestamp arrival_time);

    // Indices into the borrowed span, in the order they arrived.
    const std::vector<std::size_t>& arrival_order() const noexcept { return arrival_order_; }

    // Realized one-way delivery latency (arrival_time - true_time) per
    // delivered event, in arrival order -- parallel to arrival_order().
    const std::vector<double>& realized_latency_ns() const noexcept { return latency_ns_; }

    DeliveryStats stats() const noexcept { return stats_; }

private:
    std::span<MarketEvent>   events_;
    std::vector<std::size_t> arrival_order_;
    std::vector<double>      latency_ns_;
    DeliveryStats             stats_;

    // Per-source bookkeeping for same_source_inversions: the departure
    // (true_time) of the most recently ARRIVED event from each source, so a
    // later arrival with an earlier true_time than its own source's last
    // arrival can be recognized as an inversion.
    std::vector<NsTimestamp> last_arrived_departure_by_source_;
    std::vector<bool>        source_seen_;
};

// Schedules one source's slice of already-generated events onto `scheduler`,
// with delivery mediated by `latency` and received by `collector`.
//
// `events` is the contiguous slice belonging to a single source, in
// generation order; `indices_begin` is that slice's offset within the
// caller's full pooled vector (the same vector `collector` was constructed
// over), so on_arrival() is told the right index to stamp.
//
// Latencies are drawn for the WHOLE slice eagerly, before any scheduling --
// RNG consumption order depends only on (source, event index), never on
// heap/arrival interleaving. If apply_reordering is set, latency.apply_reordering
// permutes that latency vector afterward, which is what lets a
// later-departing event overtake an earlier one -- this is the primitive's
// first real integration point (previously exercised only by its own unit
// tests). Departure time is event.true_time (when the event actually
// occurred), not source_local_time (the source's distorted belief about
// when) -- physics must not be steered by a clock's reporting error.
void schedule_source_delivery(VirtualTimeScheduler&        scheduler,
                               AsyncCollector&               collector,
                               LatencyModel&                 latency,
                               std::span<const MarketEvent>  events,
                               std::size_t                   indices_begin,
                               bool                           apply_reordering);

} // namespace chronos
