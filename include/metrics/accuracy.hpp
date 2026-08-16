#pragma once

#include "core/types.hpp"
#include <cstdint>
#include <vector>

namespace chronos {

struct AccuracyResult {
    double      accuracy         = 1.0;
    std::size_t pairs_considered = 0; // pairs actually evaluated (excludes true_time ties)
    std::size_t total_pairs      = 0; // n * (n - 1) / 2 for the input
    bool        sampled          = false;
};

// Fraction of event pairs (i, j) where the ordering by compare_times agrees
// with the ordering by true_times. Pairs with equal true_time are excluded
// from the count. true_times[k] and compare_times[k] must refer to the same
// event k (same length, same order) -- this lets one canonical event
// ordering be reused to compare several candidate timestamp columns (e.g.
// raw vs. corrected) against the same sampled pairs, which is required for
// a fair before/after comparison.
//
// Below a fixed pair-count ceiling, every pair is checked exactly. Above it,
// this switches to a deterministic sample of pairs chosen by an RNG seeded
// from `seed`, so (true_times, compare_times, seed) always yields the same
// result and cost stays bounded instead of growing O(n^2).
AccuracyResult pairwise_ordering_accuracy(const std::vector<NsTimestamp>& true_times,
                                           const std::vector<NsTimestamp>& compare_times,
                                           std::uint64_t seed);

} // namespace chronos
