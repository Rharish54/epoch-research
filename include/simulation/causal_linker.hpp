#pragma once

#include "simulation/market_event.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chronos {

struct CausalLinkConfig {
    // Target fraction of events to enlist into causal chains (approached
    // from below -- see CausalLinkStats::events_linked for the realized
    // count). [0, 1].
    double        chain_fraction = 0.3;
    std::uint64_t seed           = 0;
};

struct CausalLinkStats {
    std::size_t chains_created   = 0;
    std::size_t events_linked    = 0; // events belonging to some chain (heads + links)
    std::size_t edges            = 0; // == events_linked - chains_created
    std::size_t abandoned_chains = 0; // chain heads with no eligible successor found
};

// Assigns cross-source causal chains (OrderSubmitted -> OrderAcknowledged ->
// OrderExecuted -> TradePublished, in that order) over an already-generated,
// pooled event list, by setting parent_event_id and rewriting event_type.
// Events not enlisted into a chain get event_type rewritten to QuoteUpdated
// or Heartbeat, so "has an order-lifecycle type" and "is a chain member"
// stay a true invariant.
//
// Mutates ONLY event_type and parent_event_id -- true_time, source_local_time,
// receive_time, event_id, and source_id are left byte-identical. This is
// required for reproducibility: every Phase 3/4 metric is a function of
// timestamps alone, and calling this must not perturb any of them.
//
// Guarantees, by construction:
//   - parent.true_time < child.true_time strictly, for every assigned edge
//   - parent.source_id != child.source_id, for every assigned edge
//   - each event has at most one parent and belongs to at most one chain
//     (the induced graph is a forest)
//
// Deterministic for a fixed (events, cfg) pair, regardless of the input
// vector's order: chain membership is decided by a forward scan over an
// internally computed (true_time, event_id) ordering, not by insertion order.
CausalLinkStats assign_causal_chains(std::vector<MarketEvent>& events,
                                      const CausalLinkConfig&   cfg);

} // namespace chronos
