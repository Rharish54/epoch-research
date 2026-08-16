#include "clock/clock_model.hpp"

namespace chronos {

namespace {
// XORed into the sync channel's seed so it never collides with the event
// channel's seed (config.seed) for any realistic input.
constexpr std::uint64_t kSyncSeedSalt = 0xD1B54A32D192ED03ULL;
} // namespace

ClockModel::ClockModel(SourceId source_id, const ClockConfig& config, NsTimestamp reference_time)
    : source_id_(source_id)
    , config_(config)
    , reference_time_(reference_time)
    , event_rng_(config.seed)
    , sync_rng_(config.seed ^ kSyncSeedSalt)
    , event_noise_dist_(0.0, config.noise_std_ns > 0.0 ? config.noise_std_ns : 1.0)
    , sync_noise_dist_(0.0, config.noise_std_ns > 0.0 ? config.noise_std_ns : 1.0)
{}

NsDuration ClockModel::sample_noise(NoiseChannel channel) {
    if (config_.noise_std_ns <= 0.0) return 0;
    switch (channel) {
        case NoiseChannel::Event: return static_cast<NsDuration>(event_noise_dist_(event_rng_));
        case NoiseChannel::Sync:  return static_cast<NsDuration>(sync_noise_dist_(sync_rng_));
    }
    return 0;
}

NsTimestamp ClockModel::to_local(NsTimestamp true_ns, NoiseChannel channel) {
    const NsDuration elapsed = true_ns - reference_time_;
    const NsDuration drift   = static_cast<NsDuration>(config_.drift_ppm * static_cast<double>(elapsed) / 1'000'000.0);
    const NsDuration noise   = sample_noise(channel);

    return true_ns + config_.offset_ns + drift + noise;
}

} // namespace chronos
