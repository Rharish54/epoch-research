#include "metrics/accuracy.hpp"

#include <gtest/gtest.h>

using namespace chronos;

TEST(Accuracy, FewerThanTwoEventsIsTriviallyPerfect) {
    EXPECT_DOUBLE_EQ(pairwise_ordering_accuracy({}, {}, 1).accuracy, 1.0);

    std::vector<NsTimestamp> one_true = {100};
    std::vector<NsTimestamp> one_cmp  = {100};
    EXPECT_DOUBLE_EQ(pairwise_ordering_accuracy(one_true, one_cmp, 1).accuracy, 1.0);
}

TEST(Accuracy, PerfectAgreementScoresOne) {
    std::vector<NsTimestamp> true_times = {100, 200, 300};
    std::vector<NsTimestamp> cmp_times  = {100, 200, 300};

    const auto result = pairwise_ordering_accuracy(true_times, cmp_times, 1);
    EXPECT_DOUBLE_EQ(result.accuracy, 1.0);
    EXPECT_EQ(result.pairs_considered, 3u); // 3 choose 2
    EXPECT_EQ(result.total_pairs, 3u);
    EXPECT_FALSE(result.sampled);
}

TEST(Accuracy, OneDiscordantPairOutOfThreeYieldsExpectedFraction) {
    // true order: 0 < 1 < 2. compare order swaps events 1 and 2.
    std::vector<NsTimestamp> true_times = {100, 200, 300};
    std::vector<NsTimestamp> cmp_times  = {100, 300, 200};

    const auto result = pairwise_ordering_accuracy(true_times, cmp_times, 1);
    // Pairs: (0,1) concordant, (0,2) concordant, (1,2) discordant -> 2/3.
    EXPECT_NEAR(result.accuracy, 2.0 / 3.0, 1e-12);
}

TEST(Accuracy, TiedTrueTimeExcludedFromPairs) {
    std::vector<NsTimestamp> true_times = {100, 100}; // tied
    std::vector<NsTimestamp> cmp_times  = {100, 999};

    const auto result = pairwise_ordering_accuracy(true_times, cmp_times, 1);
    EXPECT_EQ(result.pairs_considered, 0u);
    EXPECT_EQ(result.total_pairs, 1u); // pair exists, just doesn't count toward accuracy
    EXPECT_DOUBLE_EQ(result.accuracy, 1.0); // total==0 defaults to 1.0
}

TEST(Accuracy, ComparingTrueTimesAgainstThemselvesIsAlwaysPerfect) {
    std::vector<NsTimestamp> true_times = {300, 100, 200};
    const auto result = pairwise_ordering_accuracy(true_times, true_times, 1);
    EXPECT_DOUBLE_EQ(result.accuracy, 1.0);
}

TEST(Accuracy, SameCandidateColumnsSampleIdenticalPairsAcrossCalls) {
    // The whole point of the parallel-vectors signature: comparing two
    // different candidate columns (e.g. raw vs. corrected) against the same
    // true_times, seed, and event count must sample the identical set of
    // pairs, so a before/after delta is attributable to the columns, not to
    // sampling variance. pairs_considered (which depends only on which pairs
    // were sampled, not their values) must match exactly.
    std::vector<NsTimestamp> true_times(3000);
    std::vector<NsTimestamp> candidate_a(3000);
    std::vector<NsTimestamp> candidate_b(3000);
    for (std::size_t i = 0; i < 3000; ++i) {
        true_times[i]  = static_cast<NsTimestamp>(i);
        candidate_a[i] = static_cast<NsTimestamp>((i * 2654435761ULL) % 6000);
        candidate_b[i] = static_cast<NsTimestamp>((i * 40503ULL) % 6000);
    }

    const auto result_a = pairwise_ordering_accuracy(true_times, candidate_a, 7);
    const auto result_b = pairwise_ordering_accuracy(true_times, candidate_b, 7);

    ASSERT_TRUE(result_a.sampled);
    ASSERT_TRUE(result_b.sampled);
    EXPECT_EQ(result_a.pairs_considered, result_b.pairs_considered);
    EXPECT_EQ(result_a.total_pairs, result_b.total_pairs);
}

namespace {

std::vector<NsTimestamp> make_large_true_times(std::size_t n) {
    std::vector<NsTimestamp> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<NsTimestamp>(i);
    return v;
}

std::vector<NsTimestamp> make_large_compare_times(std::size_t n) {
    std::vector<NsTimestamp> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Deliberately decoupled from the true-time index so accuracy is
        // neither trivially 0 nor 1.
        v[i] = static_cast<NsTimestamp>((i * 2654435761ULL) % (n * 2));
    }
    return v;
}

} // namespace

TEST(Accuracy, LargeInputSwitchesToSamplingAndStaysBounded) {
    // n = 3000 -> 3000*2999/2 = 4,498,500 pairs, above the exact-computation
    // ceiling, so this must be sampled rather than iterating every pair.
    auto true_times = make_large_true_times(3000);
    auto cmp_times  = make_large_compare_times(3000);
    const auto result = pairwise_ordering_accuracy(true_times, cmp_times, 7);

    EXPECT_TRUE(result.sampled);
    EXPECT_EQ(result.total_pairs, 3000ull * 2999 / 2);
    // Sampled pair count must be bounded well below total_pairs, not scale
    // with it -- this is the whole point of the sampling fix.
    EXPECT_LT(result.pairs_considered, result.total_pairs);
    EXPECT_GT(result.pairs_considered, 0u);
}

TEST(Accuracy, SamplingIsDeterministicForSameSeed) {
    auto true_times = make_large_true_times(3000);
    auto cmp_times  = make_large_compare_times(3000);
    const auto r1 = pairwise_ordering_accuracy(true_times, cmp_times, 123);
    const auto r2 = pairwise_ordering_accuracy(true_times, cmp_times, 123);
    EXPECT_EQ(r1.pairs_considered, r2.pairs_considered);
    EXPECT_DOUBLE_EQ(r1.accuracy, r2.accuracy);
}

TEST(Accuracy, SamplingCostDoesNotGrowWithInputSizeOnceOverThreshold) {
    // Both inputs exceed the exact-pair ceiling; the number of pairs actually
    // evaluated should be comparable regardless of n, not proportional to
    // n^2 -- otherwise the O(n^2) blowup this fix targets would still exist.
    auto small_true = make_large_true_times(3000);      // ~4.5M pairs
    auto small_cmp  = make_large_compare_times(3000);
    auto large_true = make_large_true_times(50000);     // ~1.25B pairs
    auto large_cmp  = make_large_compare_times(50000);

    const auto r_small = pairwise_ordering_accuracy(small_true, small_cmp, 7);
    const auto r_large = pairwise_ordering_accuracy(large_true, large_cmp, 7);

    ASSERT_TRUE(r_small.sampled);
    ASSERT_TRUE(r_large.sampled);
    // Within a small constant factor of each other despite ~275x more total pairs.
    EXPECT_LT(static_cast<double>(r_large.pairs_considered),
              static_cast<double>(r_small.pairs_considered) * 2.0);
}
