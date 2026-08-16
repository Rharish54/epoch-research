#pragma once

#include "core/types.hpp"
#include "simulation/market_event.hpp"

namespace chronos {

// A MarketEvent's corrected global timestamp plus an uncertainty interval.
// lower_bound/upper_bound bracket the corrected_time by the offset
// estimator's uncertainty half-width (see
// DriftAwareOffsetEstimator::OffsetEstimate) and are the basis for
// ordering::classify (definite-before / definite-after / ambiguous), rather
// than comparing corrected_time directly -- forcing a total order from a
// point estimate alone would claim more precision than the estimator
// actually has.
struct CorrectedEvent {
    MarketEvent original;
    NsTimestamp corrected_time;
    NsTimestamp lower_bound;
    NsTimestamp upper_bound;
};

} // namespace chronos
