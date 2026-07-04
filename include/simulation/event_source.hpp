#pragma once

#include "core/types.hpp"
#include "clock/clock_model.hpp"
#include "simulation/market_event.hpp"
#include <cstdint>
#include <random>
#include <vector>

namespace chronos {

struct EventSourceConfig {
    NsTimestamp  start_time_ns       = 0;
    NsDuration   mean_inter_event_ns = 1'000'000; // 1 ms default
    std::uint64_t seed               = 0;
};

// Generates a stream of MarketEvents from a single simulated source.
// Event timestamps advance in true-time, then are distorted by the ClockModel.
// All randomness is seeded, so output is fully deterministic.
class EventSource {
public:
    EventSource(SourceId          source_id,
                const ClockConfig&      clock_cfg,
                const EventSourceConfig& src_cfg,
                NsTimestamp       reference_time);

    // Generate num_events events, advancing internal state each call.
    std::vector<MarketEvent> generate(std::size_t num_events);

    SourceId source_id() const noexcept { return source_id_; }

private:
    SourceId         source_id_;
    ClockModel       clock_;
    EventSourceConfig config_;

    std::mt19937_64                          rng_;
    std::exponential_distribution<double>    inter_event_dist_;
    std::uniform_int_distribution<std::size_t> type_dist_;

    std::uint64_t next_local_event_id_{0};
    NsTimestamp   current_true_time_;
};

} // namespace chronos
