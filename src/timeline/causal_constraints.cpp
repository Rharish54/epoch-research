#include "timeline/causal_constraints.hpp"

#include "timeline/interval_order.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace chronos {

ViolationReport check_point_violations(const CausalGraph& g, const std::vector<NsTimestamp>& times) {
    ViolationReport report;
    for (const auto& e : g.edges()) {
        ++report.edges_checked;
        if (times[e.child] < times[e.parent]) {
            ++report.violations;
            if (report.examples.size() < kMaxViolationExamples) report.examples.push_back(e);
        } else if (times[e.child] == times[e.parent]) {
            ++report.ties;
        }
    }
    return report;
}

ViolationReport check_interval_violations(const CausalGraph& g, const std::vector<CorrectedEvent>& events) {
    ViolationReport report;
    for (const auto& e : g.edges()) {
        ++report.edges_checked;
        const auto& parent = events[e.parent];
        const auto& child  = events[e.child];
        // No feasible tp < tc iff the child's interval can't reach past the
        // parent's lower bound -- see header for why this is <=, not <.
        if (child.upper_bound <= parent.lower_bound) {
            ++report.violations;
            if (report.examples.size() < kMaxViolationExamples) report.examples.push_back(e);
        }
    }
    return report;
}

namespace {

// Union-find over node indices, used to group nodes into weakly-connected
// components when an infeasible chain is found. Path compression only (no
// union-by-rank) -- graphs here are small forests, so this is not a
// bottleneck, and the simpler version is easier to verify by inspection.
class UnionFind {
public:
    explicit UnionFind(std::size_t n) : parent_(n) {
        for (std::size_t i = 0; i < n; ++i) parent_[i] = i;
    }
    std::size_t find(std::size_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }
    void unite(std::size_t a, std::size_t b) { parent_[find(a)] = find(b); }

private:
    std::vector<std::size_t> parent_;
};

} // namespace

std::optional<NarrowingResult> narrow_intervals(const CausalGraph& g, std::vector<CorrectedEvent>& events) {
    const auto topo = g.topological_order();
    if (!topo) return std::nullopt;

    const std::size_t n = g.size();
    NarrowingResult result;
    result.edge_violations = check_interval_violations(g, events).violations;

    std::vector<NsTimestamp> lo(n), hi(n);
    for (std::size_t i = 0; i < n; ++i) {
        lo[i] = events[i].lower_bound;
        hi[i] = events[i].upper_bound;
    }

    // Forward pass: lift each node's lower bound above every parent's.
    for (std::size_t node : *topo) {
        for (std::size_t parent : g.parents_of(node)) {
            lo[node] = std::max(lo[node], static_cast<NsTimestamp>(lo[parent] + kCausalEpsilonNs));
        }
    }
    // Reverse pass: lower each node's upper bound below every child's.
    for (auto it = topo->rbegin(); it != topo->rend(); ++it) {
        for (std::size_t child : g.children_of(*it)) {
            hi[*it] = std::min(hi[*it], static_cast<NsTimestamp>(hi[child] - kCausalEpsilonNs));
        }
    }

    // Group nodes touched by any causal edge into weakly-connected
    // components, and mark a component inconsistent if narrowing produced
    // an empty interval anywhere in it.
    UnionFind uf(n);
    for (const auto& e : g.edges()) uf.unite(e.parent, e.child);

    std::unordered_set<std::size_t> inconsistent_roots;
    for (std::size_t i = 0; i < n; ++i) {
        if (lo[i] > hi[i]) inconsistent_roots.insert(uf.find(i));
    }

    for (std::size_t i = 0; i < n; ++i) {
        // Only nodes that participate in at least one edge have a
        // meaningful component root distinct from themselves-as-singleton;
        // isolated nodes (no causal edges at all) are never touched by
        // narrowing and are skipped entirely.
        const bool touched = !g.parents_of(i).empty() || !g.children_of(i).empty();
        if (!touched) continue;

        if (inconsistent_roots.count(uf.find(i)) > 0) {
            ++result.nodes_reverted;
            continue; // leave events[i] bounds exactly as they were
        }

        const bool changed = lo[i] != events[i].lower_bound || hi[i] != events[i].upper_bound;
        events[i].lower_bound = lo[i];
        events[i].upper_bound = hi[i];
        events[i].corrected_time = std::clamp(events[i].corrected_time, lo[i], hi[i]);
        if (changed) ++result.nodes_narrowed;
    }

    result.inconsistent_components = inconsistent_roots.size();
    return result;
}

namespace {

// All ancestors of `node` reachable via parent edges (BFS, dedup via
// visited set) -- not just its immediate parent. For the forests
// assign_causal_chains produces this is just the chain prefix above `node`,
// but the walk is written generically over CausalGraph's parents_of, which
// supports multiple parents.
std::vector<std::size_t> ancestors_of(const CausalGraph& g, std::size_t node) {
    std::vector<std::size_t> result;
    std::unordered_set<std::size_t> visited;
    std::vector<std::size_t> frontier = {node};
    while (!frontier.empty()) {
        std::vector<std::size_t> next;
        for (std::size_t n : frontier) {
            for (std::size_t p : g.parents_of(n)) {
                if (visited.insert(p).second) {
                    result.push_back(p);
                    next.push_back(p);
                }
            }
        }
        frontier = std::move(next);
    }
    return result;
}

} // namespace

CausalPairResult evaluate_causal_pairs(const CausalGraph&                 g,
                                        const std::vector<CorrectedEvent>& before,
                                        const std::vector<CorrectedEvent>& after) {
    CausalPairResult result;
    const std::size_t n = g.size();

    for (std::size_t child = 0; child < n; ++child) {
        for (std::size_t ancestor : ancestors_of(g, child)) {
            ++result.pairs;

            const bool true_ancestor_first = before[ancestor].original.true_time < before[child].original.true_time;

            const auto verdict_before = classify_order(before[ancestor], before[child]);
            if (verdict_before == TemporalOrder::Ambiguous) {
                ++result.ambiguous_before;
            } else {
                ++result.definite_before;
                const bool says_ancestor_first = verdict_before == TemporalOrder::Before;
                if (says_ancestor_first == true_ancestor_first) ++result.definite_correct_before;
            }

            const auto verdict_after = classify_order(after[ancestor], after[child]);
            if (verdict_after == TemporalOrder::Ambiguous) {
                ++result.ambiguous_after;
            } else {
                ++result.definite_after;
                const bool says_ancestor_first = verdict_after == TemporalOrder::Before;
                if (says_ancestor_first == true_ancestor_first) ++result.definite_correct_after;
            }
        }
    }

    return result;
}

} // namespace chronos
