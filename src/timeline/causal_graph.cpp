#include "timeline/causal_graph.hpp"

#include <deque>

namespace chronos {

template <typename IdFn, typename ParentFn>
CausalGraph CausalGraph::build_from(std::size_t n, IdFn id_of, ParentFn parent_of) {
    CausalGraph g;
    g.children_.assign(n, {});
    g.parents_.assign(n, {});

    for (std::size_t i = 0; i < n; ++i) {
        const auto key = static_cast<std::uint64_t>(id_of(i));
        if (!g.index_by_id_.emplace(key, i).second) {
            ++g.duplicate_ids_; // first occurrence wins; this one is not indexed
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        const auto parent_id = parent_of(i);
        if (!parent_id.has_value()) continue;

        const auto it = g.index_by_id_.find(static_cast<std::uint64_t>(*parent_id));
        if (it == g.index_by_id_.end()) {
            ++g.dangling_parents_;
            continue;
        }

        const std::size_t parent_idx = it->second;
        g.children_[parent_idx].push_back(i);
        g.parents_[i].push_back(parent_idx);
        g.edges_.push_back(CausalGraph::Edge{parent_idx, i});
    }

    return g;
}

CausalGraph CausalGraph::build(const std::vector<MarketEvent>& events) {
    return build_from(
        events.size(),
        [&](std::size_t i) { return events[i].event_id; },
        [&](std::size_t i) { return events[i].parent_event_id; });
}

CausalGraph CausalGraph::build(const std::vector<CorrectedEvent>& events) {
    return build_from(
        events.size(),
        [&](std::size_t i) { return events[i].original.event_id; },
        [&](std::size_t i) { return events[i].original.parent_event_id; });
}

std::optional<std::size_t> CausalGraph::index_of(EventId id) const {
    const auto it = index_by_id_.find(static_cast<std::uint64_t>(id));
    if (it == index_by_id_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::vector<std::size_t>> CausalGraph::topological_order() const {
    const std::size_t n = children_.size();
    std::vector<std::size_t> in_degree(n, 0);
    for (const auto& e : edges_) ++in_degree[e.child];

    std::deque<std::size_t> queue;
    for (std::size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) queue.push_back(i);
    }

    std::vector<std::size_t> order;
    order.reserve(n);
    while (!queue.empty()) {
        const std::size_t node = queue.front();
        queue.pop_front();
        order.push_back(node);
        for (std::size_t child : children_[node]) {
            if (--in_degree[child] == 0) queue.push_back(child);
        }
    }

    if (order.size() != n) return std::nullopt; // a cycle left some node(s) unreachable
    return order;
}

} // namespace chronos
