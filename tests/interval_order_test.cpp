#include "timeline/interval_order.hpp"

#include <gtest/gtest.h>

using namespace chronos;

namespace {

CorrectedEvent make_event(NsTimestamp lower, NsTimestamp corrected, NsTimestamp upper) {
    CorrectedEvent e{};
    e.corrected_time = corrected;
    e.lower_bound     = lower;
    e.upper_bound     = upper;
    return e;
}

} // namespace

TEST(IntervalOrder, NonOverlappingIntervalsClassifyBefore) {
    const auto a = make_event(0, 100, 200);
    const auto b = make_event(300, 400, 500);
    EXPECT_EQ(classify_order(a, b), TemporalOrder::Before);
    EXPECT_EQ(classify_order(b, a), TemporalOrder::After);
}

TEST(IntervalOrder, OverlappingIntervalsAreAmbiguous) {
    const auto a = make_event(0, 100, 300);
    const auto b = make_event(200, 400, 600);
    EXPECT_EQ(classify_order(a, b), TemporalOrder::Ambiguous);
    EXPECT_EQ(classify_order(b, a), TemporalOrder::Ambiguous);
}

TEST(IntervalOrder, OneIntervalFullyInsideAnotherIsAmbiguous) {
    const auto outer = make_event(0, 500, 1000);
    const auto inner = make_event(400, 500, 600);
    EXPECT_EQ(classify_order(outer, inner), TemporalOrder::Ambiguous);
    EXPECT_EQ(classify_order(inner, outer), TemporalOrder::Ambiguous);
}

TEST(IntervalOrder, TouchingBoundsAreAmbiguousNotDefinite) {
    // a.upper_bound == b.lower_bound exactly -- a tie, not a strict separation.
    const auto a = make_event(0, 100, 200);
    const auto b = make_event(200, 300, 400);
    EXPECT_EQ(classify_order(a, b), TemporalOrder::Ambiguous);
    EXPECT_EQ(classify_order(b, a), TemporalOrder::Ambiguous);
}

TEST(IntervalOrder, IdenticalIntervalsAreAmbiguous) {
    const auto a = make_event(100, 150, 200);
    const auto b = make_event(100, 150, 200);
    EXPECT_EQ(classify_order(a, b), TemporalOrder::Ambiguous);
}

TEST(IntervalOrder, ZeroWidthNonOverlappingIntervalsClassifyDefinite) {
    const auto a = make_event(100, 100, 100);
    const auto b = make_event(200, 200, 200);
    EXPECT_EQ(classify_order(a, b), TemporalOrder::Before);
    EXPECT_EQ(classify_order(b, a), TemporalOrder::After);
}
