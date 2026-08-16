#include "clock/offset_estimator.hpp"

namespace chronos {

void OffsetEstimator::add_sample(const SyncSample& sample) {
    const NsTimestamp key = (sample.collector_send_time + sample.collector_receive_time) / 2;
    samples_[key] = sample.estimated_offset;
}

std::optional<NsDuration> OffsetEstimator::estimate_at(NsTimestamp query_time) const {
    auto it = samples_.upper_bound(query_time);
    if (it == samples_.begin()) return std::nullopt;
    --it;
    return it->second;
}

} // namespace chronos
