#pragma once

#include "core/types.hpp"
#include "simulation/market_event.hpp"
#include "timeline/corrected_event.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace chronos {

// Directed dependency graph built from MarketEvent::parent_event_id links
// over a fixed, indexed event list. Node index i is that event's position in
// the vector the graph was built from -- callers keep parallel arrays
// (timestamps, corrected bounds) in that same index space.
//
// Since parent_event_id is a single std::optional, in-degree is always <= 1:
// the graph produced by assign_causal_chains is always a forest. Cycle
// detection is still implemented as a guard against hand-built/corrupt
// input (see tests) rather than something the linker can ever produce.
class CausalGraph {
public:
    struct Edge {
        std::size_t parent;
        std::size_t child;
    };

    static CausalGraph build(const std::vector<MarketEvent>& events);
    static CausalGraph build(const std::vector<CorrectedEvent>& events);

    std::size_t size() const noexcept { return children_.size(); }
    const std::vector<Edge>& edges() const noexcept { return edges_; }

    const std::vector<std::size_t>& children_of(std::size_t node) const { return children_[node]; }
    const std::vector<std::size_t>& parents_of(std::size_t node) const { return parents_[node]; }

    std::optional<std::size_t> index_of(EventId id) const;

    // parent_event_id values with no matching event in the input list.
    std::size_t dangling_parents() const noexcept { return dangling_parents_; }

    // event_id values that appeared more than once in the input list (the
    // first occurrence wins for indexing; duplicates are not silently merged).
    std::size_t duplicate_ids() const noexcept { return duplicate_ids_; }

    // Kahn's algorithm, FIFO seeded in ascending index order for a
    // deterministic result. std::nullopt iff the graph contains a cycle.
    std::optional<std::vector<std::size_t>> topological_order() const;
    bool has_cycle() const { return !topological_order().has_value(); }

private:
    CausalGraph() = default;

    // Shared construction logic for both build() overloads. `n` events,
    // `id_of(i)`/`parent_of(i)` give the i-th event's own id and its
    // optional parent id. A member template rather than a free function so
    // it can populate the private fields directly.
    template <typename IdFn, typename ParentFn>
    static CausalGraph build_from(std::size_t n, IdFn id_of, ParentFn parent_of);

    std::vector<std::vector<std::size_t>> children_;
    std::vector<std::vector<std::size_t>> parents_;
    std::vector<Edge>                     edges_;
    std::unordered_map<std::uint64_t, std::size_t> index_by_id_;
    std::size_t dangling_parents_ = 0;
    std::size_t duplicate_ids_    = 0;
};

} // namespace chronos
