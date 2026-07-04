#include "simulation/event_source.hpp"

#include <array>
#include <cstddef>

namespace chronos {

namespace {
constexpr std::array<EventType, 6> kEventTypes = {
    EventType::OrderSubmitted,
    EventType::OrderAcknowledged,
    EventType::OrderExecuted,
    EventType::QuoteUpdated,
    EventType::TradePublished,
    EventType::Heartbeat,
};
} // namespace

EventSource::EventSource(SourceId                 source_id,
                         const ClockConfig&       clock_cfg,
                         const EventSourceConfig& src_cfg,
                         NsTimestamp              reference_time)
    : source_id_(source_id)
    , clock_(source_id, clock_cfg, reference_time)
    , config_(src_cfg)
    , rng_(src_cfg.seed)
    // exponential_distribution rate = 1/mean
    , inter_event_dist_(1.0 / static_cast<double>(src_cfg.mean_inter_event_ns))
    , type_dist_(0, kEventTypes.size() - 1)
    , current_true_time_(src_cfg.start_time_ns)
{}

std::vector<MarketEvent> EventSource::generate(std::size_t num_events) {
    std::vector<MarketEvent> events;
    events.reserve(num_events);

    for (std::size_t i = 0; i < num_events; ++i) {
        // Advance true time by a Poisson-distributed gap.
        const auto gap = static_cast<NsDuration>(inter_event_dist_(rng_));
        current_true_time_ += (gap > 0 ? gap : 1); // guard against zero gap

        MarketEvent evt;
        evt.event_id        = static_cast<EventId>(next_local_event_id_++);
        evt.source_id       = source_id_;
        evt.event_type      = kEventTypes[type_dist_(rng_)];
        evt.true_time       = current_true_time_;
        evt.source_local_time = clock_.to_local(current_true_time_);
        evt.receive_time    = 0; // populated in Phase 6
        events.push_back(evt);
    }

    return events;
}

} // namespace chronos
