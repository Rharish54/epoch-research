#pragma once

#include "core/types.hpp"
#include "timeline/causal_graph.hpp"
#include "timeline/corrected_event.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace chronos {

// Smallest gap that separates two events on this integer-nanosecond
// timeline. Used as the strict-inequality margin in both violation checks
// and interval narrowing, so the two agree exactly at the boundary.
inline constexpr NsDuration kCausalEpsilonNs = 1;

constexpr std::size_t kMaxViolationExamples = 8;

struct ViolationReport {
    std::size_t edges_checked = 0;
    std::size_t violations    = 0;
    std::size_t ties          = 0; // point mode only: exactly-equal timestamps
    std::vector<CausalGraph::Edge> examples; // first kMaxViolationExamples violating edges
};

// Point-timeline check: `times[i]` is the single timestamp for node i.
// Violation: times[child] < times[parent] -- the timeline positively
// asserts the child happened first. times[child] == times[parent] is
// counted as a tie, not a violation: equal timestamps fail to witness the
// causal order, but don't contradict it either.
ViolationReport check_point_violations(const CausalGraph& g, const std::vector<NsTimestamp>& times);

// Interval-timeline check. Violation: child.upper_bound <= parent.lower_bound,
// i.e. no assignment tp in [parent.lower, parent.upper], tc in
// [child.lower, child.upper] satisfies tp < tc.
//
// Note this is `<=`, not the strict `<` that classify_order (Phase 4) uses:
// classify_order's Before/After split is about which event's interval sits
// entirely earlier, and a shared boundary point is Ambiguous there because
// either event could plausibly be the earlier one. Here the question is
// different -- "can parent < child be satisfied at all" -- and at
// child.upper == parent.lower, the only feasible assignment is
// tc == tp, which still violates strict causal precedence.
ViolationReport check_interval_violations(const CausalGraph& g, const std::vector<CorrectedEvent>& events);

struct NarrowingResult {
    std::size_t edge_violations         = 0; // from check_interval_violations, pre-narrowing
    std::size_t nodes_narrowed          = 0; // nodes whose lower and/or upper bound moved
    std::size_t inconsistent_components = 0; // weakly-connected components with no feasible assignment
    std::size_t nodes_reverted          = 0; // nodes in those components, left at their original bounds
};

// Tightens `events`' bounds in place using the causal edges of `g` (arc
// consistency on the "parent strictly before child" constraint), by:
//   forward pass (topological order): child.lower  = max(child.lower,  parent.lower + eps)
//   reverse pass (reverse topo order): parent.upper = min(parent.upper, child.upper - eps)
// corrected_time is then clamped into the (possibly narrowed) [lower, upper].
//
// Soundness: if every input interval already contains its own true_time,
// narrowing can never push a bound past that true_time (see README for the
// inductive argument), so a correct interval cannot be made incorrect by
// this pass alone. This does NOT mean the result is trustworthy in
// general -- Phase 4's uncertainty is a heuristic bound, not a
// coverage-guaranteed confidence interval, so intervals that miss their
// true_time can propagate that error along a chain. This is measured, not
// assumed away -- see the README's Phase 5 section.
//
// A node whose narrowed lower bound exceeds its narrowed upper bound
// signals a chain whose causal constraints and timing intervals are jointly
// infeasible. Rather than clamp such a node to an empty or inverted
// interval (which would make classify_order report a confident wrong
// answer), every node in that node's weakly-connected component is
// reverted to its pre-narrowing bounds and excluded from any "narrowed"
// claim; inconsistent_components/nodes_reverted count this.
//
// Requires g.size() == events.size(). Returns std::nullopt if g has a cycle
// (topological_order() failed) -- narrowing is undefined without a
// topological order to sweep in.
std::optional<NarrowingResult> narrow_intervals(const CausalGraph& g, std::vector<CorrectedEvent>& events);

struct CausalPairResult {
    std::size_t pairs                    = 0; // ancestor/descendant pairs evaluated
    std::size_t ambiguous_before         = 0;
    std::size_t ambiguous_after          = 0;
    std::size_t definite_before          = 0;
    std::size_t definite_after           = 0;
    std::size_t definite_correct_before  = 0;
    std::size_t definite_correct_after   = 0;
};

// Exact (not sampled) evaluation of classify_order restricted to
// ancestor/descendant pairs related by `g`, comparing `before` (e.g.
// pre-narrowing) against `after` (post-narrowing). `before` and `after`
// must be the same events in the same index order as `g` was built from --
// only their bounds are expected to differ.
//
// This exists because Phase 4's classify_pairs samples uniformly over ALL
// event pairs, and most events in a run are temporally far apart and
// trivially ordered regardless of uncertainty -- narrowing's real effect,
// concentrated on the causally-linked minority of events, gets diluted
// into noise by that sampling. Walking the graph's own edges instead
// evaluates exactly the pairs narrowing could plausibly affect.
CausalPairResult evaluate_causal_pairs(const CausalGraph&                 g,
                                        const std::vector<CorrectedEvent>& before,
                                        const std::vector<CorrectedEvent>& after);

} // namespace chronos
