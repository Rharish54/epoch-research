#include "metrics/ambiguity.hpp"

#include "timeline/interval_order.hpp"

#include <random>
#include <utility>

namespace chronos {

namespace {
// Mirrors metrics/accuracy.cpp's ceiling so both metrics scale the same way
// on the same run.
constexpr std::size_t kMaxExactPairs = 2'000'000;
} // namespace

AmbiguityResult classify_pairs(const std::vector<CorrectedEvent>& events, std::uint64_t seed) {
    AmbiguityResult result;
    const std::size_t n = events.size();
    if (n < 2) return result;

    result.total_pairs = n * (n - 1) / 2;

    auto check_pair = [&](std::size_t i, std::size_t j) {
        const auto& a = events[i];
        const auto& b = events[j];
        if (a.original.true_time == b.original.true_time) return; // skip simultaneous events

        const bool true_order_a_first = a.original.true_time < b.original.true_time;

        const TemporalOrder verdict = classify_order(a, b);
        if (verdict == TemporalOrder::Ambiguous) {
            ++result.ambiguous_pairs;
        } else {
            ++result.definite_pairs;
            const bool interval_says_a_first = verdict == TemporalOrder::Before;
            if (interval_says_a_first == true_order_a_first) ++result.definite_correct;
        }

        const bool point_says_a_first = a.corrected_time < b.corrected_time;
        if (point_says_a_first != true_order_a_first) {
            ++result.point_estimate_errors;
            if (verdict == TemporalOrder::Ambiguous) ++result.point_estimate_errors_flagged_ambiguous;
        }

        ++result.pairs_considered;
    };

    if (result.total_pairs <= kMaxExactPairs) {
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                check_pair(i, j);
            }
        }
    } else {
        result.sampled = true;
        // Same XOR constant as pairwise_ordering_accuracy so this metric's
        // sampling stream is independent of that one, but both remain
        // reproducible from the same top-level seed.
        std::mt19937_64 rng(seed ^ 0xA5A5A5A5A5A5A5A5ULL);
        std::uniform_int_distribution<std::size_t> dist(0, n - 1);
        for (std::size_t s = 0; s < kMaxExactPairs; ++s) {
            std::size_t i = dist(rng);
            std::size_t j = dist(rng);
            if (i == j) continue;
            if (i > j) std::swap(i, j);
            check_pair(i, j);
        }
    }

    return result;
}

} // namespace chronos
