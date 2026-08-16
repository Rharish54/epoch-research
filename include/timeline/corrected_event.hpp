#pragma once

#include "core/types.hpp"
#include "simulation/market_event.hpp"

namespace chronos {

// A MarketEvent's corrected global timestamp. Confidence-interval fields
// (lower_bound, upper_bound) are added in Phase 4; only the point estimate
// is populated here.
struct CorrectedEvent {
    MarketEvent original;
    NsTimestamp corrected_time;
};

} // namespace chronos
