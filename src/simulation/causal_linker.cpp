#include "simulation/causal_linker.hpp"

#include <algorithm>
#include <array>
#include <random>

namespace chronos {

namespace {

// Order-lifecycle template a chain's members are assigned, by position.
constexpr std::array<EventType, 4> kChainTemplate = {
    EventType::OrderSubmitted,
    EventType::OrderAcknowledged,
    EventType::OrderExecuted,
    EventType::TradePublished,
};

// How far forward (in the true-time-sorted order) to search for the next
// eligible chain successor before giving up on extending a chain. Bounded so
// linking stays O(n * kMaxLookahead) rather than O(n^2) in pathological
// all-one-source runs.
constexpr std::size_t kMaxLookahead = 256;

// Number of eligible-but-skipped candidates to pass over before taking one,
// for gap variety in which events get linked (purely cosmetic -- it does not
// affect any correctness guarantee).
constexpr std::size_t kLinkSkipMax = 3;

} // namespace

CausalLinkStats assign_causal_chains(std::vector<MarketEvent>& events, const CausalLinkConfig& cfg) {
    CausalLinkStats stats;
    const std::size_t n = events.size();
    if (n == 0 || cfg.chain_fraction <= 0.0) return stats;

    // order[k] = index into `events` of the k-th event in (true_time, event_id)
    // order. event_id is a strict tiebreak so this ordering is total and
    // independent of events' original (insertion) order.
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (events[a].true_time != events[b].true_time) return events[a].true_time < events[b].true_time;
        return static_cast<std::uint64_t>(events[a].event_id) < static_cast<std::uint64_t>(events[b].event_id);
    });

    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<int> chain_len_dist(2, 4);   // template has 4 entries
    std::uniform_int_distribution<std::size_t> skip_dist(0, kLinkSkipMax);

    std::vector<bool> used(n, false);
    const auto target_linked = static_cast<std::size_t>(cfg.chain_fraction * static_cast<double>(n));

    for (std::size_t pos = 0; pos < n && stats.events_linked < target_linked; ++pos) {
        const std::size_t head_idx = order[pos];
        if (used[head_idx]) continue;

        const int chain_len = std::min<int>(chain_len_dist(rng),
            static_cast<int>(target_linked - stats.events_linked));
        if (chain_len < 2) break; // not enough budget left for even a 2-member chain

        // Grow the chain: from each member's position in `order`, scan
        // forward within kMaxLookahead for the next unused event on a
        // different source with a strictly later true_time.
        std::vector<std::size_t> members = {head_idx};
        std::size_t search_from = pos;
        for (int step = 1; step < chain_len; ++step) {
            const std::size_t prev_idx = members.back();
            std::size_t skip_remaining = skip_dist(rng);
            std::size_t found = n; // sentinel: not found

            const std::size_t window_end = std::min(n, search_from + 1 + kMaxLookahead);
            for (std::size_t j = search_from + 1; j < window_end; ++j) {
                const std::size_t cand_idx = order[j];
                if (used[cand_idx]) continue;
                if (events[cand_idx].source_id == events[prev_idx].source_id) continue;
                if (events[cand_idx].true_time <= events[prev_idx].true_time) continue;

                if (skip_remaining > 0) {
                    --skip_remaining;
                    continue;
                }
                found = j;
                break;
            }

            if (found == n) break; // no eligible successor within the window
            members.push_back(order[found]);
            search_from = found;
        }

        if (members.size() < 2) {
            ++stats.abandoned_chains;
            continue; // head stays unused, free to seed a future chain or stay unlinked
        }

        for (std::size_t k = 0; k < members.size(); ++k) {
            const std::size_t idx = members[k];
            used[idx] = true;
            events[idx].event_type = kChainTemplate[k];
            events[idx].parent_event_id =
                k == 0 ? std::nullopt : std::optional<EventId>(events[members[k - 1]].event_id);
        }

        ++stats.chains_created;
        stats.events_linked += members.size();
        stats.edges += members.size() - 1;
    }

    // Second pass: any event not enlisted into a chain gets a non-chain
    // type, so "order-lifecycle type" <=> "chain member" is a real invariant.
    for (std::size_t i = 0; i < n; ++i) {
        if (used[i]) continue;
        events[i].event_type =
            (static_cast<std::uint64_t>(events[i].event_id) & 1) ? EventType::QuoteUpdated : EventType::Heartbeat;
    }

    return stats;
}

} // namespace chronos
