#include "clock/clock_model.hpp"

namespace chronos {

ClockModel::ClockModel(SourceId source_id, const ClockConfig& config, NsTimestamp reference_time)
    : source_id_(source_id)
    , config_(config)
    , reference_time_(reference_time)
    , rng_(config.seed)
    , noise_dist_(0.0, config.noise_std_ns > 0.0 ? config.noise_std_ns : 1.0)
{}

NsTimestamp ClockModel::to_local(NsTimestamp true_ns) {
    const NsDuration elapsed = true_ns - reference_time_;
    const NsDuration drift   = static_cast<NsDuration>(config_.drift_ppm * static_cast<double>(elapsed) / 1'000'000.0);

    NsDuration noise = 0;
    if (config_.noise_std_ns > 0.0) {
        noise = static_cast<NsDuration>(noise_dist_(rng_));
    }

    return true_ns + config_.offset_ns + drift + noise;
}

} // namespace chronos
