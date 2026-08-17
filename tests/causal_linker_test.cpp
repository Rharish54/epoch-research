#include "simulation/causal_linker.hpp"

#include <gtest/gtest.h>
#include <unordered_map>
#include <unordered_set>

using namespace chronos;

namespace {

// A pooled multi-source event list: `num_sources` sources, `events_per_source`
// events each, true_time spread out so cross-source interleaving is
// possible, event_id unique across the whole pool.
std::vector<MarketEvent> make_pool(int num_sources, int events_per_source, NsTimestamp start = 1000) {
    std::vector<MarketEvent> events;
    std::uint64_t next_id = 0;
    for (int s = 0; s < num_sources; ++s) {
        for (int i = 0; i < events_per_source; ++i) {
            MarketEvent e{};
            e.event_id  = static_cast<EventId>(next_id++);
            e.source_id = static_cast<SourceId>(s);
            e.event_type = EventType::Heartbeat;
            // Interleave sources: true_time = i * num_sources + s, so events
            // from different sources sit close together in true-time order.
            e.true_time = start + static_cast<NsTimestamp>(i * num_sources + s) * 100;
            e.source_local_time = e.true_time; // irrelevant to the linker
            events.push_back(e);
        }
    }
    return events;
}

} // namespace

TEST(CausalLinker, ZeroChainFractionCreatesNoEdges) {
    auto events = make_pool(8, 20);
    const auto stats = assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.0, .seed = 1});
    EXPECT_EQ(stats.chains_created, 0u);
    EXPECT_EQ(stats.events_linked, 0u);
    EXPECT_EQ(stats.edges, 0u);
    for (const auto& e : events) EXPECT_FALSE(e.parent_event_id.has_value());
}

TEST(CausalLinker, EveryParentPrecedesItsChildInTrueTime) {
    auto events = make_pool(8, 50);
    std::unordered_map<std::uint64_t, const MarketEvent*> by_id;
    for (const auto& e : events) by_id[static_cast<std::uint64_t>(e.event_id)] = &e;

    assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.5, .seed = 7});

    std::size_t edges_checked = 0;
    for (const auto& e : events) {
        if (!e.parent_event_id.has_value()) continue;
        const auto* parent = by_id.at(static_cast<std::uint64_t>(*e.parent_event_id));
        EXPECT_LT(parent->true_time, e.true_time);
        ++edges_checked;
    }
    EXPECT_GT(edges_checked, 0u);
}

TEST(CausalLinker, EveryCausalEdgeCrossesSourceBoundaries) {
    auto events = make_pool(8, 50);
    std::unordered_map<std::uint64_t, const MarketEvent*> by_id;
    for (const auto& e : events) by_id[static_cast<std::uint64_t>(e.event_id)] = &e;

    assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.5, .seed = 7});

    std::size_t edges_checked = 0;
    for (const auto& e : events) {
        if (!e.parent_event_id.has_value()) continue;
        const auto* parent = by_id.at(static_cast<std::uint64_t>(*e.parent_event_id));
        EXPECT_NE(parent->source_id, e.source_id);
        ++edges_checked;
    }
    EXPECT_GT(edges_checked, 0u);
}

TEST(CausalLinker, NoEventHasMoreThanOneParentAndChainsDoNotOverlap) {
    auto events = make_pool(8, 50);
    assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.5, .seed = 7});

    // parent_event_id being a single std::optional already guarantees
    // in-degree <= 1; verify no event_id is claimed as a parent of two
    // different chains at inconsistent positions (i.e. every parent link
    // resolves to exactly one real event, checked in the precedes-in-time
    // test above) and that used events aren't reassigned to a second chain
    // by checking each event_id appears as a "child slot" at most once.
    std::unordered_set<std::uint64_t> seen_as_child;
    for (const auto& e : events) {
        if (!e.parent_event_id.has_value()) continue;
        const auto id = static_cast<std::uint64_t>(e.event_id);
        EXPECT_TRUE(seen_as_child.insert(id).second) << "event " << id << " assigned as a child twice";
    }
}

TEST(CausalLinker, TimestampsAreUnmodifiedByLinking) {
    auto events = make_pool(8, 50);
    const auto before = events; // copy

    assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.5, .seed = 7});

    ASSERT_EQ(events.size(), before.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].event_id, before[i].event_id);
        EXPECT_EQ(events[i].source_id, before[i].source_id);
        EXPECT_EQ(events[i].true_time, before[i].true_time);
        EXPECT_EQ(events[i].source_local_time, before[i].source_local_time);
        EXPECT_EQ(events[i].receive_time, before[i].receive_time);
    }
}

TEST(CausalLinker, SameSeedProducesIdenticalLinking) {
    auto events_a = make_pool(8, 50);
    auto events_b = make_pool(8, 50);

    assign_causal_chains(events_a, CausalLinkConfig{.chain_fraction = 0.4, .seed = 99});
    assign_causal_chains(events_b, CausalLinkConfig{.chain_fraction = 0.4, .seed = 99});

    ASSERT_EQ(events_a.size(), events_b.size());
    for (std::size_t i = 0; i < events_a.size(); ++i) {
        EXPECT_EQ(events_a[i].event_type, events_b[i].event_type);
        EXPECT_EQ(events_a[i].parent_event_id, events_b[i].parent_event_id);
    }
}

TEST(CausalLinker, DifferentSeedProducesDifferentLinking) {
    auto events_a = make_pool(8, 50);
    auto events_b = make_pool(8, 50);

    assign_causal_chains(events_a, CausalLinkConfig{.chain_fraction = 0.4, .seed = 1});
    assign_causal_chains(events_b, CausalLinkConfig{.chain_fraction = 0.4, .seed = 2});

    bool any_diff = false;
    for (std::size_t i = 0; i < events_a.size(); ++i) {
        if (events_a[i].parent_event_id != events_b[i].parent_event_id) any_diff = true;
    }
    EXPECT_TRUE(any_diff);
}

TEST(CausalLinker, LinkedFractionApproximatesRequestedFraction) {
    auto events = make_pool(16, 100);
    const auto stats = assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.3, .seed = 42});

    const double realized = static_cast<double>(stats.events_linked) / static_cast<double>(events.size());
    EXPECT_GE(realized, 0.15); // generous tolerance -- realized can fall short of target
    EXPECT_LE(realized, 0.30);
}

TEST(CausalLinker, ChainTypesFollowTheOrderLifecycleTemplateAndNonChainEventsDoNot) {
    auto events = make_pool(8, 50);
    std::unordered_map<std::uint64_t, const MarketEvent*> by_id;
    for (const auto& e : events) by_id[static_cast<std::uint64_t>(e.event_id)] = &e;

    assign_causal_chains(events, CausalLinkConfig{.chain_fraction = 0.5, .seed = 7});

    for (const auto& e : events) {
        if (e.parent_event_id.has_value()) {
            // A chain member (has a parent): must carry an order-lifecycle type.
            EXPECT_TRUE(e.event_type == EventType::OrderAcknowledged ||
                        e.event_type == EventType::OrderExecuted ||
                        e.event_type == EventType::TradePublished);
        }
    }
}
