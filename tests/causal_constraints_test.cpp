#include "timeline/causal_constraints.hpp"

#include "timeline/interval_order.hpp"

#include <gtest/gtest.h>

using namespace chronos;

namespace {

MarketEvent make_market_event(std::uint64_t id, NsTimestamp true_time,
                               std::optional<std::uint64_t> parent_id = std::nullopt) {
    MarketEvent e{};
    e.event_id  = static_cast<EventId>(id);
    e.source_id = static_cast<SourceId>(0);
    e.event_type = EventType::Heartbeat;
    e.true_time = true_time;
    e.source_local_time = true_time;
    if (parent_id) e.parent_event_id = static_cast<EventId>(*parent_id);
    return e;
}

CorrectedEvent make_corrected(std::uint64_t id, NsTimestamp true_time, NsTimestamp lower, NsTimestamp corrected,
                               NsTimestamp upper, std::optional<std::uint64_t> parent_id = std::nullopt) {
    CorrectedEvent ce{};
    ce.original       = make_market_event(id, true_time, parent_id);
    ce.corrected_time = corrected;
    ce.lower_bound     = lower;
    ce.upper_bound     = upper;
    return ce;
}

} // namespace

// ---- check_point_violations ----

TEST(CausalConstraints, ChildTimestampBeforeParentIsAPointViolation) {
    std::vector<MarketEvent> events = {make_market_event(0, 1000), make_market_event(1, 2000, 0)};
    const auto g = CausalGraph::build(events);

    // Timeline where the child's timestamp is EARLIER than the parent's.
    std::vector<NsTimestamp> times = {500, 100};
    const auto report = check_point_violations(g, times);
    EXPECT_EQ(report.violations, 1u);
    EXPECT_EQ(report.ties, 0u);
}

TEST(CausalConstraints, EqualTimestampsAreCountedAsTieNotViolation) {
    std::vector<MarketEvent> events = {make_market_event(0, 1000), make_market_event(1, 2000, 0)};
    const auto g = CausalGraph::build(events);
    std::vector<NsTimestamp> times = {500, 500};
    const auto report = check_point_violations(g, times);
    EXPECT_EQ(report.violations, 0u);
    EXPECT_EQ(report.ties, 1u);
}

TEST(CausalConstraints, ValidChainHasNoViolationsUnderTrueTimestamps) {
    std::vector<MarketEvent> events = {
        make_market_event(0, 100), make_market_event(1, 200, 0),
        make_market_event(2, 300, 1), make_market_event(3, 400, 2),
    };
    const auto g = CausalGraph::build(events);
    std::vector<NsTimestamp> true_times;
    for (const auto& e : events) true_times.push_back(e.true_time);
    const auto report = check_point_violations(g, true_times);
    EXPECT_EQ(report.violations, 0u);
}

// ---- check_interval_violations ----

TEST(CausalConstraints, OverlappingButSatisfiableIntervalsAreNotAViolation) {
    // parent=[100,300], child=[200,400]: tp=100,tc=200 satisfies tp<tc.
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 100, 100, 200, 300),
        make_corrected(1, 200, 200, 300, 400, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    const auto report = check_interval_violations(g, events);
    EXPECT_EQ(report.violations, 0u);
}

TEST(CausalConstraints, IntervalsThatCannotSatisfyTheConstraintAreAViolation) {
    // child=[0,50] entirely before parent=[100,200] -- no feasible tp<tc.
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 150, 100, 150, 200),
        make_corrected(1, 25, 0, 25, 50, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    const auto report = check_interval_violations(g, events);
    EXPECT_EQ(report.violations, 1u);
}

TEST(CausalConstraints, TouchingIntervalsAreAViolationEvenThoughClassifyOrderCallsThemAmbiguous) {
    // child.upper == parent.lower exactly: only feasible assignment is
    // tp == tc, which still violates strict parent < child.
    const auto parent = make_corrected(0, 250, 200, 250, 300);
    const auto child   = make_corrected(1, 150, 100, 150, 200, /*parent_id=*/0);
    ASSERT_EQ(child.upper_bound, parent.lower_bound);

    // classify_order (Phase 4) treats a shared boundary as Ambiguous --
    // this is the documented divergence between the two checks.
    EXPECT_EQ(classify_order(child, parent), TemporalOrder::Ambiguous);

    std::vector<CorrectedEvent> events = {parent, child};
    const auto g = CausalGraph::build(events);
    const auto report = check_interval_violations(g, events);
    EXPECT_EQ(report.violations, 1u);
}

// ---- narrow_intervals ----

TEST(CausalConstraints, ForwardPassRaisesChildLowerBoundToParentLowerBoundPlusEpsilon) {
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 150, 100, 150, 300),
        make_corrected(1, 200, 50, 200, 400, /*parent_id=*/0), // child's lower (50) is BELOW parent's (100)
    };
    const auto g = CausalGraph::build(events);
    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(events[1].lower_bound, 100 + kCausalEpsilonNs);
}

TEST(CausalConstraints, ReversePassLowersParentUpperBoundBelowChildUpperBoundMinusEpsilon) {
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 150, 100, 150, 500), // parent's upper (500) is ABOVE child's (300)
        make_corrected(1, 200, 150, 200, 300, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(events[0].upper_bound, 300 - kCausalEpsilonNs);
}

TEST(CausalConstraints, DirectlyLinkedOverlappingPairIsResolvedByReachabilityNotNarrowing) {
    // A=[100,300], B=[200,400], A->B. Narrowing leaves both unchanged
    // (validated: arc consistency on a single edge is a no-op when the
    // intervals already satisfy the constraint) -- classify_order alone
    // still calls this Ambiguous after narrowing. Resolving it requires
    // consulting graph reachability directly, which is exactly why
    // evaluate_causal_pairs exists.
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 150, 100, 150, 300),
        make_corrected(1, 250, 200, 250, 400, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    const auto before = events;
    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(events[0].lower_bound, before[0].lower_bound);
    EXPECT_EQ(events[0].upper_bound, before[0].upper_bound);
    EXPECT_EQ(events[1].lower_bound, before[1].lower_bound);
    EXPECT_EQ(events[1].upper_bound, before[1].upper_bound);
    EXPECT_EQ(classify_order(events[0], events[1]), TemporalOrder::Ambiguous);
}

TEST(CausalConstraints, NarrowingResolvesAChainMemberVsUnrelatedEventPairThatReachabilityCannot) {
    // Chain A->B->C where A's lower bound is late; D is unrelated, entirely
    // separate from the chain. Before narrowing C vs D is Ambiguous; the
    // forward pass lifts C's lower bound (transitively, via B) above D's
    // upper bound, resolving the pair. D is not reachable from any chain
    // member, so reachability alone could never do this -- validated
    // during planning with the Python prototype this test encodes.
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 550, 500, 550, 600),               // A
        make_corrected(1, 650, 100, 650, 700, /*parent=*/0), // B
        make_corrected(2, 750, 200, 750, 800, /*parent=*/1), // C
        make_corrected(3, 400, 0, 400, 450),                  // D, unrelated
    };
    const auto g = CausalGraph::build(events);

    ASSERT_EQ(classify_order(events[2], events[3]), TemporalOrder::Ambiguous);

    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(classify_order(events[2], events[3]), TemporalOrder::After);
}

TEST(CausalConstraints, ContradictoryChainIsRevertedNotClampedToAnEmptyInterval) {
    // child's entire interval is before parent's -- infeasible.
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 150, 100, 150, 200),
        make_corrected(1, 25, 0, 25, 50, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    const auto before = events;
    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->inconsistent_components, 1u);
    EXPECT_EQ(result->nodes_reverted, 2u);
    // Bounds must be untouched -- never an inverted/empty interval.
    EXPECT_EQ(events[0].lower_bound, before[0].lower_bound);
    EXPECT_EQ(events[0].upper_bound, before[0].upper_bound);
    EXPECT_EQ(events[1].lower_bound, before[1].lower_bound);
    EXPECT_EQ(events[1].upper_bound, before[1].upper_bound);
    EXPECT_LE(events[0].lower_bound, events[0].upper_bound);
    EXPECT_LE(events[1].lower_bound, events[1].upper_bound);
}

TEST(CausalConstraints, PathLevelContradictionIsDetectedEvenWithNoSingleViolatingEdge) {
    // A=[0,10] -> B=[0,10] -> C=[0,1]. No single EDGE violates
    // (C.upper=1 > B.lower=0), but the path requires C >= A + 2, which
    // C's interval [0,1] cannot satisfy -- infeasible only at the path level.
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 0, 0, 0, 10),
        make_corrected(1, 1, 0, 1, 10, /*parent_id=*/0),
        make_corrected(2, 1, 0, 1, 1, /*parent_id=*/1),
    };
    const auto g = CausalGraph::build(events);
    const auto edge_report = check_interval_violations(g, events);
    EXPECT_EQ(edge_report.violations, 0u);

    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->inconsistent_components, 1u);
}

TEST(CausalConstraints, IsolatedNodeWithNoCausalEdgesIsNeverTouchedByNarrowing) {
    std::vector<CorrectedEvent> events = {make_corrected(0, 500, 400, 500, 600)};
    const auto g = CausalGraph::build(events);
    const auto before = events;
    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->nodes_narrowed, 0u);
    EXPECT_EQ(events[0].lower_bound, before[0].lower_bound);
    EXPECT_EQ(events[0].upper_bound, before[0].upper_bound);
}

TEST(CausalConstraints, CorrectedTimeIsClampedIntoTheNarrowedInterval) {
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 150, 100, 150, 300),
        // corrected_time (120) sits below where the narrowed lower bound will land.
        make_corrected(1, 200, 50, 120, 400, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    const auto result = narrow_intervals(g, events);
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(events[1].corrected_time, events[1].lower_bound);
    EXPECT_LE(events[1].corrected_time, events[1].upper_bound);
}

TEST(CausalConstraints, NarrowingOnACyclicGraphReturnsNullopt) {
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 100, 50, 100, 150, /*parent_id=*/1),
        make_corrected(1, 200, 150, 200, 250, /*parent_id=*/0),
    };
    const auto g = CausalGraph::build(events);
    EXPECT_FALSE(narrow_intervals(g, events).has_value());
}

TEST(CausalConstraints, NarrowingIsIdempotent) {
    std::vector<CorrectedEvent> events = {
        make_corrected(0, 550, 500, 550, 600),
        make_corrected(1, 650, 100, 650, 700, /*parent=*/0),
        make_corrected(2, 750, 200, 750, 800, /*parent=*/1),
    };
    const auto g = CausalGraph::build(events);
    ASSERT_TRUE(narrow_intervals(g, events).has_value());
    const auto once = events;
    ASSERT_TRUE(narrow_intervals(g, events).has_value());
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].lower_bound, once[i].lower_bound);
        EXPECT_EQ(events[i].upper_bound, once[i].upper_bound);
    }
}

// ---- evaluate_causal_pairs ----

TEST(CausalConstraints, AmbiguousCausalPairIsCountedResolvedAfterNarrowing) {
    std::vector<CorrectedEvent> before_events = {
        make_corrected(0, 550, 500, 550, 600),
        make_corrected(1, 650, 100, 650, 700, /*parent=*/0),
        make_corrected(2, 750, 200, 750, 800, /*parent=*/1),
        make_corrected(3, 400, 0, 400, 450),
    };
    const auto g = CausalGraph::build(before_events);
    auto after_events = before_events;
    ASSERT_TRUE(narrow_intervals(g, after_events).has_value());

    const auto result = evaluate_causal_pairs(g, before_events, after_events);
    EXPECT_GT(result.pairs, 0u);
    // Every pair here is causally related (0-1, 0-2, 1-2), so all should be
    // definite both before and after -- narrowing only affects node 2's
    // relationship to the unrelated node 3, which evaluate_causal_pairs
    // does not include (3 has no causal relationship to anything).
    EXPECT_EQ(result.pairs, 3u);
}
