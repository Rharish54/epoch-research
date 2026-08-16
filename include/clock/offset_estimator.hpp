#pragma once

#include "clock/sync_sample.hpp"
#include "core/types.hpp"
#include <map>
#include <optional>

namespace chronos {

// Baseline per-source offset estimator: returns the most recently observed
// sync sample's estimated_offset at or before the query time. Deliberately
// naive -- under clock drift this leaves a residual error equal to drift
// accumulated since that sample, which Phase 3's sliding-window drift
// estimator addresses. Do not add drift-awareness here.
//
// Samples are keyed by the midpoint of their collector-time window,
// (collector_send_time + collector_receive_time) / 2 -- the only sync
// timestamp on the same clock as the true/collector time used elsewhere.
// Callers query with an event's source_local_time, a different clock, so
// near a sync boundary a query can resolve to the neighboring sample.
// Offsets are small relative to the sync interval, so this is a negligible
// approximation, not a hidden bug.
class OffsetEstimator {
public:
    // Records a sync sample. Call order does not matter -- samples are
    // stored keyed by time, not by insertion sequence.
    void add_sample(const SyncSample& sample);

    // Returns the offset from the most recent sample whose key is <=
    // query_time, or std::nullopt if no such sample has been added.
    std::optional<NsDuration> estimate_at(NsTimestamp query_time) const;

    std::size_t sample_count() const noexcept { return samples_.size(); }

private:
    std::map<NsTimestamp, NsDuration> samples_; // key: collector-time midpoint
};

} // namespace chronos
