#include "network/latency_model.hpp"

#include <algorithm>
#include <utility>

namespace chronos {

LatencyModel::LatencyModel(const LatencyConfig& config)
    : config_(config)
    , rng_(config.seed)
    , jitter_dist_(0.0, config.jitter_std_ns > 0.0 ? config.jitter_std_ns : 1.0)
    , spike_dist_(config.spike_probability)
    , reorder_dist_(config.reorder_probability)
{}

NsDuration LatencyModel::sample_latency(LinkDirection direction) {
    // Outbound gets the +asymmetry/2 half, inbound the -asymmetry/2 half, so
    // (outbound - inbound) == asymmetry_ns exactly, matching the offset
    // estimator's error analysis (error == asymmetry / 2).
    const double sign     = direction == LinkDirection::Outbound ? 1.0 : -1.0;
    const double dir_base = static_cast<double>(config_.base_ns)
                           + sign * static_cast<double>(config_.asymmetry_ns) / 2.0;

    double latency = dir_base;
    if (config_.jitter_std_ns > 0.0) {
        latency += jitter_dist_(rng_);
    }
    if (config_.spike_probability > 0.0 && spike_dist_(rng_)) {
        latency *= config_.spike_multiplier;
    }

    latency = std::max(latency, 1.0);
    return static_cast<NsDuration>(latency);
}

void LatencyModel::apply_reordering(std::vector<NsDuration>& latencies) {
    if (config_.reorder_probability <= 0.0) return;

    for (std::size_t i = 0; i + 1 < latencies.size(); i += 2) {
        if (reorder_dist_(rng_)) {
            std::swap(latencies[i], latencies[i + 1]);
        }
    }
}

} // namespace chronos
