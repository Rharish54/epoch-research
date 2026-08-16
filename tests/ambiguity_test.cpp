#include "metrics/ambiguity.hpp"

#include <gtest/gtest.h>

using namespace chronos;

namespace {

CorrectedEvent make_event(NsTimestamp true_time, NsTimestamp lower, NsTimestamp corrected, NsTimestamp upper) {
    CorrectedEvent e{};
    e.original.true_time = true_time;
    e.corrected_time      = corrected;
    e.lower_bound          = lower;
    e.upper_bound          = upper;
    return e;
}

} // namespace

TEST(Ambiguity, TooFewEventsReturnsZeroedResult) {
    std::vector<CorrectedEvent> events = {make_event(0, 0, 0, 0)};
    const auto result = classify_pairs(events, /*seed=*/1);
    EXPECT_EQ(result.total_pairs, 0u);
    EXPECT_EQ(result.pairs_considered, 0u);
}

TEST(Ambiguity, TwoNonOverlappingCorrectlyOrderedEventsAreDefiniteAndCorrect) {
    // true order matches interval order.
    std::vector<CorrectedEvent> events = {
        make_event(/*true=*/100, /*lower=*/0,   /*corr=*/100, /*upper=*/200),
        make_event(/*true=*/500, /*lower=*/300, /*corr=*/400, /*upper=*/500),
    };
    const auto result = classify_pairs(events, /*seed=*/1);
    EXPECT_EQ(result.pairs_considered, 1u);
    EXPECT_EQ(result.definite_pairs, 1u);
    EXPECT_EQ(result.ambiguous_pairs, 0u);
    EXPECT_EQ(result.definite_correct, 1u);
    EXPECT_EQ(result.point_estimate_errors, 0u);
}

TEST(Ambiguity, OverlappingIntervalsCountAsAmbiguousNotDefinite) {
    std::vector<CorrectedEvent> events = {
        make_event(/*true=*/100, /*lower=*/0,   /*corr=*/100, /*upper=*/300),
        make_event(/*true=*/200, /*lower=*/200, /*corr=*/400, /*upper=*/600),
    };
    const auto result = classify_pairs(events, /*seed=*/1);
    EXPECT_EQ(result.definite_pairs, 0u);
    EXPECT_EQ(result.ambiguous_pairs, 1u);
}

TEST(Ambiguity, DefiniteButWrongOrderIsNotCountedCorrect) {
    // Intervals don't overlap (definite), but the definite order disagrees
    // with true_time -- e.g. a badly biased offset estimate.
    std::vector<CorrectedEvent> events = {
        make_event(/*true=*/500, /*lower=*/0,   /*corr=*/100, /*upper=*/200), // true_time later, interval earlier
        make_event(/*true=*/100, /*lower=*/300, /*corr=*/400, /*upper=*/500),
    };
    const auto result = classify_pairs(events, /*seed=*/1);
    EXPECT_EQ(result.definite_pairs, 1u);
    EXPECT_EQ(result.definite_correct, 0u);
}

TEST(Ambiguity, PointEstimateErrorCaughtByAmbiguityFlagIsCounted) {
    // corrected_time ordering disagrees with true_time (a point-estimate
    // mistake), but the intervals overlap, so classify_order correctly
    // hedges instead of asserting the wrong order.
    std::vector<CorrectedEvent> events = {
        make_event(/*true=*/500, /*lower=*/50, /*corr=*/100, /*upper=*/450), // corrected_time says "first"
        make_event(/*true=*/100, /*lower=*/150, /*corr=*/400, /*upper=*/500), // but true_time says "first" is the other one
    };
    const auto result = classify_pairs(events, /*seed=*/1);
    EXPECT_EQ(result.ambiguous_pairs, 1u);
    EXPECT_EQ(result.point_estimate_errors, 1u);
    EXPECT_EQ(result.point_estimate_errors_flagged_ambiguous, 1u);
}

TEST(Ambiguity, SimultaneousTrueTimeEventsAreExcluded) {
    std::vector<CorrectedEvent> events = {
        make_event(/*true=*/100, /*lower=*/0,   /*corr=*/100, /*upper=*/200),
        make_event(/*true=*/100, /*lower=*/300, /*corr=*/400, /*upper=*/500),
    };
    const auto result = classify_pairs(events, /*seed=*/1);
    EXPECT_EQ(result.pairs_considered, 0u);
    EXPECT_EQ(result.definite_pairs, 0u);
    EXPECT_EQ(result.ambiguous_pairs, 0u);
}
