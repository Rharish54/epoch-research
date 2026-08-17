#include "timeline/causal_graph.hpp"

#include <gtest/gtest.h>

using namespace chronos;

namespace {

MarketEvent make_event(std::uint64_t id, std::optional<std::uint64_t> parent_id = std::nullopt) {
    MarketEvent e{};
    e.event_id  = static_cast<EventId>(id);
    e.source_id = static_cast<SourceId>(0);
    e.event_type = EventType::Heartbeat;
    e.true_time = static_cast<NsTimestamp>(id) * 100;
    e.source_local_time = e.true_time;
    if (parent_id) e.parent_event_id = static_cast<EventId>(*parent_id);
    return e;
}

} // namespace

TEST(CausalGraph, ChainOfFourProducesTopologicalOrderMatchingChainOrder) {
    std::vector<MarketEvent> events = {
        make_event(0),
        make_event(1, 0),
        make_event(2, 1),
        make_event(3, 2),
    };
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.edges().size(), 3u);

    const auto topo = g.topological_order();
    ASSERT_TRUE(topo.has_value());
    ASSERT_EQ(topo->size(), 4u);
    EXPECT_EQ(*topo, (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(CausalGraph, TopologicalOrderPlacesEveryParentBeforeItsChild) {
    std::vector<MarketEvent> events = {
        make_event(0),
        make_event(1),
        make_event(2, 0),
        make_event(3, 1),
        make_event(4, 2),
    };
    const auto g = CausalGraph::build(events);
    const auto topo = g.topological_order();
    ASSERT_TRUE(topo.has_value());

    std::vector<std::size_t> position(topo->size());
    for (std::size_t pos = 0; pos < topo->size(); ++pos) position[(*topo)[pos]] = pos;

    for (const auto& e : g.edges()) {
        EXPECT_LT(position[e.parent], position[e.child]);
    }
}

TEST(CausalGraph, DisconnectedNodesAppearExactlyOnceInTopologicalOrder) {
    std::vector<MarketEvent> events = {make_event(0), make_event(1), make_event(2)};
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.edges().size(), 0u);
    const auto topo = g.topological_order();
    ASSERT_TRUE(topo.has_value());
    EXPECT_EQ(topo->size(), 3u);
}

TEST(CausalGraph, MultipleChildrenOfOneParentAllFollowIt) {
    std::vector<MarketEvent> events = {
        make_event(0),
        make_event(1, 0),
        make_event(2, 0),
        make_event(3, 0),
    };
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.children_of(0).size(), 3u);
    const auto topo = g.topological_order();
    ASSERT_TRUE(topo.has_value());
    EXPECT_EQ(topo->front(), 0u);
}

TEST(CausalGraph, CycleIsDetectedAndTopologicalOrderIsNullopt) {
    // Hand-built cycle: 0 -> 1 -> 0. Unreachable from assign_causal_chains
    // (which only ever points a child at a strictly-earlier event), but
    // CausalGraph must still guard against corrupt/hand-built input.
    std::vector<MarketEvent> events = {
        make_event(0, 1),
        make_event(1, 0),
    };
    const auto g = CausalGraph::build(events);
    EXPECT_FALSE(g.topological_order().has_value());
    EXPECT_TRUE(g.has_cycle());
}

TEST(CausalGraph, DanglingParentIdIsCountedNotTreatedAsAnEdge) {
    std::vector<MarketEvent> events = {make_event(5, /*parent_id=*/999)};
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.edges().size(), 0u);
    EXPECT_EQ(g.dangling_parents(), 1u);
}

TEST(CausalGraph, DuplicateEventIdsAreCountedNotSilentlyMerged) {
    std::vector<MarketEvent> events = {make_event(0), make_event(0)};
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.duplicate_ids(), 1u);
}

TEST(CausalGraph, IndexOfFindsAnAssignedEventAndNulloptForUnknownId) {
    std::vector<MarketEvent> events = {make_event(0), make_event(1, 0)};
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.index_of(static_cast<EventId>(1)), 1u);
    EXPECT_FALSE(g.index_of(static_cast<EventId>(42)).has_value());
}

TEST(CausalGraph, EmptyEventListProducesEmptyGraph) {
    std::vector<MarketEvent> events;
    const auto g = CausalGraph::build(events);
    EXPECT_EQ(g.size(), 0u);
    const auto topo = g.topological_order();
    ASSERT_TRUE(topo.has_value());
    EXPECT_TRUE(topo->empty());
}
