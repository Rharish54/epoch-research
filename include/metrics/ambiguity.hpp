#pragma once

#include "timeline/corrected_event.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chronos {

struct AmbiguityResult {
    std::size_t pairs_considered = 0; // pairs evaluated (excludes true_time ties)
    std::size_t total_pairs      = 0; // n * (n - 1) / 2 for the input
    bool        sampled          = false;

    std::size_t definite_pairs   = 0; // classify_order returned Before/After
    std::size_t ambiguous_pairs  = 0; // classify_order returned Ambiguous

    // Of the definite pairs, how many agreed with true_time ordering. This
    // should be very high -- when Chronos claims a definite order, that
    // claim needs to actually be trustworthy.
    std::size_t definite_correct = 0;

    // Pairs where the point-estimate correction (corrected_time alone, no
    // interval) disagrees with true_time ordering -- i.e. mistakes a naive
    // point-estimate timeline would make silently.
    std::size_t point_estimate_errors = 0;

    // Of those point-estimate errors, how many classify_order correctly
    // flagged as Ambiguous rather than asserting a (wrong) definite order.
    // This is the metric that validates the interval logic is doing its
    // job: catching the cases where a bare point estimate would mislead.
    std::size_t point_estimate_errors_flagged_ambiguous = 0;
};

// Evaluates ordering behavior over pairs of `events`, each of which must
// carry ground-truth original.true_time (evaluation-only, per CLAUDE.md).
// Uses the same bounded-sampling strategy as pairwise_ordering_accuracy: all
// pairs are checked below a fixed ceiling, otherwise a deterministic sample
// keyed by `seed` is used so cost stays bounded for large runs.
AmbiguityResult classify_pairs(const std::vector<CorrectedEvent>& events, std::uint64_t seed);

} // namespace chronos
