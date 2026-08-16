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

// Which independent noise stream to draw from. Event generation and the sync
// exchange (Phase 2) both call to_local(), and interleaving a shared RNG
// between them would make results depend on how often each is called --
// breaking reproducibility of anything already measured. Each channel gets
// its own generator, both seeded deterministically from config.seed.
enum class NoiseChannel { Event, Sync };

// Models a single source clock:
//   local_time = true_time + offset + drift_ppm * elapsed_ns / 1e6 + gaussian_noise
//
// reference_time is the true_time at which elapsed_ns is zero (i.e. simulation start).
// All conversions are deterministic for a fixed seed and call sequence.
class ClockModel {
public:
    ClockModel(SourceId source_id, const ClockConfig& config, NsTimestamp reference_time);

    // Convert a true timestamp to the distorted local timestamp, drawing
    // noise from the given channel.
    // NOTE: if noise_std_ns > 0, this advances that channel's RNG -- call
    // order within a channel matters for reproducibility; the two channels
    // never affect each other.
    NsTimestamp to_local(NsTimestamp true_ns, NoiseChannel channel = NoiseChannel::Event);

    SourceId          source_id()  const noexcept { return source_id_; }
    const ClockConfig& config()    const noexcept { return config_; }

private:
    NsDuration sample_noise(NoiseChannel channel);

    SourceId     source_id_;
    ClockConfig  config_;
    NsTimestamp  reference_time_;

    std::mt19937_64                    event_rng_;
    std::mt19937_64                    sync_rng_;
    std::normal_distribution<double>   event_noise_dist_;
    std::normal_distribution<double>   sync_noise_dist_;
};

} // namespace chronos
