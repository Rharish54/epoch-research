#pragma once

#include "core/types.hpp"
#include <cstdint>
#include <random>

namespace chronos {

struct ClockConfig {
    NsDuration offset_ns    = 0;      // initial offset: local_t = true_t + offset + ...
    double     drift_ppm    = 0.0;    // clock drift in parts-per-million of elapsed time
    double     noise_std_ns = 0.0;    // gaussian noise std-dev applied per sample
    std::uint64_t seed      = 0;
};

// Models a single source clock:
//   local_time = true_time + offset + drift_ppm * elapsed_ns / 1e6 + gaussian_noise
//
// reference_time is the true_time at which elapsed_ns is zero (i.e. simulation start).
// All conversions are deterministic for a fixed seed and call sequence.
class ClockModel {
public:
    ClockModel(SourceId source_id, const ClockConfig& config, NsTimestamp reference_time);

    // Convert a true timestamp to the distorted local timestamp.
    // NOTE: if noise_std_ns > 0, this advances the RNG — call order matters for reproducibility.
    NsTimestamp to_local(NsTimestamp true_ns);

    SourceId          source_id()  const noexcept { return source_id_; }
    const ClockConfig& config()    const noexcept { return config_; }

private:
    SourceId     source_id_;
    ClockConfig  config_;
    NsTimestamp  reference_time_;

    std::mt19937_64                    rng_;
    std::normal_distribution<double>   noise_dist_;
};

} // namespace chronos
