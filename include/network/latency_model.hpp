#pragma once

#include "core/types.hpp"
#include <cstdint>
#include <random>
#include <vector>

namespace chronos {

struct LatencyConfig {
    NsDuration base_ns             = 100'000;  // one-way base latency, ns (100us default)
    NsDuration asymmetry_ns        = 0;        // outbound - inbound base delay, ns
    double     jitter_std_ns       = 50'000.0; // gaussian jitter std-dev, one-way, ns
    double     spike_probability   = 0.0;      // [0,1] chance a given sample is a latency spike
    double     spike_multiplier    = 10.0;     // spike latency = (base + jitter) * multiplier
    // Not wired into the CLI or SyncExchange in Phase 2: there is no delivery
    // queue to reorder against until Phase 6's async collector exists. Kept
    // here and tested as a standalone primitive (apply_reordering) so Phase 6
    // has it ready rather than needing a network-model rewrite.
    double     reorder_probability = 0.0;
    std::uint64_t seed             = 0;
};

// Direction of a one-way network hop. In the four-timestamp sync exchange:
// Outbound is the request (collector -> source, t1 -> t2), Inbound is the
// response (source -> collector, t3 -> t4).
enum class LinkDirection { Outbound, Inbound };

// Models one-way network latency between a collector and a single source.
// All randomness is seeded, so output is fully deterministic for a fixed
// seed and call sequence.
class LatencyModel {
public:
    explicit LatencyModel(const LatencyConfig& config);

    // Draws a one-way latency sample in ns for the given direction. Always
    // >= 1ns -- real latency can't be zero or negative, so large negative
    // jitter is clamped rather than allowed to produce a nonsensical delay.
    NsDuration sample_latency(LinkDirection direction);

    // Swaps each non-overlapping adjacent pair (0,1), (2,3), ... in `latencies`
    // with probability reorder_probability, modeling packets that arrive out
    // of send order. No-op if reorder_probability <= 0.
    void apply_reordering(std::vector<NsDuration>& latencies);

    const LatencyConfig& config() const noexcept { return config_; }

private:
    LatencyConfig config_;

    std::mt19937_64                    rng_;
    std::normal_distribution<double>   jitter_dist_;
    std::bernoulli_distribution        spike_dist_;
    std::bernoulli_distribution        reorder_dist_;
};

} // namespace chronos
