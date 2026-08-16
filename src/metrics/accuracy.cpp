#include "metrics/accuracy.hpp"

#include <random>
#include <utility>

namespace chronos {

namespace {
// Bounds the cost of pairwise_ordering_accuracy. Below this many pairs, every
// pair is checked exactly; above it, exactly this many pairs are sampled.
constexpr std::size_t kMaxExactPairs = 2'000'000;
} // namespace

AccuracyResult pairwise_ordering_accuracy(const std::vector<NsTimestamp>& true_times,
                                           const std::vector<NsTimestamp>& compare_times,
                                           std::uint64_t seed) {
    AccuracyResult result;
    const std::size_t n = true_times.size();
    if (n < 2) return result;

    result.total_pairs = n * (n - 1) / 2;

    std::size_t concordant = 0;
    std::size_t total      = 0;

    auto check_pair = [&](std::size_t i, std::size_t j) {
        if (true_times[i] == true_times[j]) return; // skip simultaneous events

        const bool true_order = true_times[i] < true_times[j];
        const bool cmp_order  = compare_times[i] < compare_times[j];

        if (true_order == cmp_order) ++concordant;
        ++total;
    };

    if (result.total_pairs <= kMaxExactPairs) {
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                check_pair(i, j);
            }
        }
    } else {
        result.sampled = true;
        // XOR with a fixed constant so sampling noise is independent of the
        // clock/event-gap RNGs elsewhere in the simulation, which also derive
        // their seeds from the same top-level --seed value.
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

    result.pairs_considered = total;
    result.accuracy = total == 0 ? 1.0 : static_cast<double>(concordant) / static_cast<double>(total);
    return result;
}

} // namespace chronos
