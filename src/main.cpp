#include "clock/drift_estimator.hpp"
#include "clock/offset_estimator.hpp"
#include "metrics/accuracy.hpp"
#include "metrics/ambiguity.hpp"
#include "metrics/percentile.hpp"
#include "network/async_collector.hpp"
#include "network/latency_model.hpp"
#include "network/virtual_scheduler.hpp"
#include "simulation/causal_linker.hpp"
#include "simulation/event_source.hpp"
#include "simulation/sync_exchange.hpp"
#include "timeline/causal_constraints.hpp"
#include "timeline/causal_graph.hpp"
#include "timeline/corrected_event.hpp"

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct SimConfig {
    int         num_sources         = 16;
    int         total_events        = 5000;
    std::uint64_t seed              = 42;
    double      drift_ppm           = 300.0;
    double      max_offset_us       = 2000.0;  // each source offset varies up to ±this
    double      noise_std_ns        = 50.0;
    double      inter_event_us      = 10.0;    // mean inter-event time per source in μs
    double      sync_interval_us    = 200.0;   // gap between sync exchanges, per source
    double      base_latency_us     = 100.0;   // one-way network base latency
    double      latency_asymmetry_us= 0.0;     // outbound - inbound one-way delay
    double      jitter_us           = 50.0;    // one-way latency jitter std-dev
    double      spike_probability   = 0.0;     // [0,1] chance a latency sample spikes
    double      spike_multiplier    = 10.0;    // spike latency = (base+jitter) * multiplier
    int         drift_window        = 10;      // samples retained by the weighted drift estimator
    double      outlier_rtt_multiplier = 3.0;  // reject sync samples with RTT > multiplier * rolling median
    double      causal_chain_fraction = 0.3;   // target fraction of events enlisted into causal chains
    double      reorder_probability  = 0.0;    // [0,1] chance an adjacent async-delivery pair swaps
    std::string output_path;                   // empty = stdout only
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --sources N              number of event sources      (default 16)\n"
              << "  --events N               total events to generate     (default 5000)\n"
              << "  --seed N                 RNG seed                     (default 42)\n"
              << "  --drift-ppm F            clock drift in PPM           (default 300.0)\n"
              << "  --max-offset-us F        max abs clock offset in us   (default 2000.0)\n"
              << "  --noise-std-ns F         timestamp noise std-dev ns   (default 50.0)\n"
              << "  --inter-event-us F       mean inter-event time in us  (default 10.0)\n"
              << "  --sync-interval-us F     gap between sync exchanges   (default 200.0)\n"
              << "  --base-latency-us F      one-way network base latency (default 100.0)\n"
              << "  --latency-asymmetry-us F outbound - inbound delay     (default 0.0)\n"
              << "  --jitter-us F            one-way latency jitter stdev (default 50.0)\n"
              << "  --spike-probability F    chance a latency sample spikes, [0,1] (default 0.0)\n"
              << "  --spike-multiplier F     spike latency multiplier     (default 10.0)\n"
              << "  --drift-window N         samples in weighted drift fit (default 10)\n"
              << "  --outlier-rtt-multiplier F reject RTT > multiplier * rolling median (default 3.0)\n"
              << "  --causal-chain-fraction F target fraction of events in causal chains (default 0.3)\n"
              << "  --reorder-probability F  chance an adjacent async-delivery pair swaps (default 0.0)\n"
              << "  --output PATH            write events CSV to PATH\n";
}

// Parses `raw` via `convert`, exiting with a clear message instead of
// crashing on an uncaught std::invalid_argument/std::out_of_range.
template <typename T, typename Convert>
T parse_or_exit(std::string_view key, std::string_view raw, Convert convert) {
    try {
        return convert(std::string(raw));
    } catch (const std::exception&) {
        std::cerr << "Invalid value for " << key << ": '" << raw << "' is not a valid number\n";
        std::exit(1);
    }
}

// std::stoull silently wraps a leading '-' into a huge unsigned value rather
// than failing, so negative input is rejected explicitly before parsing.
std::uint64_t parse_u64_or_exit(std::string_view key, std::string_view raw) {
    if (!raw.empty() && raw[0] == '-') {
        std::cerr << "Invalid value for " << key << ": '" << raw << "' must not be negative\n";
        std::exit(1);
    }
    return parse_or_exit<std::uint64_t>(key, raw, [](const std::string& s) { return std::stoull(s); });
}

SimConfig parse_args(int argc, char** argv) {
    SimConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view key{argv[i]};
        auto next = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << key << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        auto dbl = [&]() { return parse_or_exit<double>(key, next(), [](const std::string& s) { return std::stod(s); }); };

        if (key == "--sources") {
            cfg.num_sources = parse_or_exit<int>(key, next(), [](const std::string& s) { return std::stoi(s); });
        } else if (key == "--events") {
            cfg.total_events = parse_or_exit<int>(key, next(), [](const std::string& s) { return std::stoi(s); });
        } else if (key == "--seed") {
            cfg.seed = parse_u64_or_exit(key, next());
        } else if (key == "--drift-ppm") {
            cfg.drift_ppm = dbl();
        } else if (key == "--max-offset-us") {
            cfg.max_offset_us = dbl();
        } else if (key == "--noise-std-ns") {
            cfg.noise_std_ns = dbl();
        } else if (key == "--inter-event-us") {
            cfg.inter_event_us = dbl();
        } else if (key == "--sync-interval-us") {
            cfg.sync_interval_us = dbl();
        } else if (key == "--base-latency-us") {
            cfg.base_latency_us = dbl();
        } else if (key == "--latency-asymmetry-us") {
            cfg.latency_asymmetry_us = dbl();
        } else if (key == "--jitter-us") {
            cfg.jitter_us = dbl();
        } else if (key == "--spike-probability") {
            cfg.spike_probability = dbl();
        } else if (key == "--spike-multiplier") {
            cfg.spike_multiplier = dbl();
        } else if (key == "--drift-window") {
            cfg.drift_window = parse_or_exit<int>(key, next(), [](const std::string& s) { return std::stoi(s); });
        } else if (key == "--outlier-rtt-multiplier") {
            cfg.outlier_rtt_multiplier = dbl();
        } else if (key == "--causal-chain-fraction") {
            cfg.causal_chain_fraction = dbl();
        } else if (key == "--reorder-probability") {
            cfg.reorder_probability = dbl();
        } else if (key == "--output") {
            cfg.output_path = std::string(next());
        } else if (key == "--help" || key == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << key << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    if (cfg.num_sources <= 0 || cfg.total_events <= 0) {
        std::cerr << "--sources and --events must be positive\n";
        std::exit(1);
    }
    if (cfg.drift_ppm < 0.0) {
        std::cerr << "--drift-ppm must not be negative (it's a magnitude; sign alternates per source)\n";
        std::exit(1);
    }
    if (cfg.max_offset_us < 0.0) {
        std::cerr << "--max-offset-us must not be negative (it's a magnitude bound)\n";
        std::exit(1);
    }
    if (cfg.noise_std_ns < 0.0) {
        std::cerr << "--noise-std-ns must not be negative\n";
        std::exit(1);
    }
    if (cfg.inter_event_us < 0.0) {
        std::cerr << "--inter-event-us must not be negative\n";
        std::exit(1);
    }
    if (cfg.sync_interval_us <= 0.0) {
        std::cerr << "--sync-interval-us must be positive (a non-positive interval produces no sync samples)\n";
        std::exit(1);
    }
    if (cfg.base_latency_us < 0.0) {
        std::cerr << "--base-latency-us must not be negative\n";
        std::exit(1);
    }
    if (cfg.jitter_us < 0.0) {
        std::cerr << "--jitter-us must not be negative\n";
        std::exit(1);
    }
    if (cfg.spike_probability < 0.0 || cfg.spike_probability > 1.0) {
        std::cerr << "--spike-probability must be within [0, 1]\n";
        std::exit(1);
    }
    if (cfg.drift_window < 1) {
        std::cerr << "--drift-window must be at least 1\n";
        std::exit(1);
    }
    if (cfg.outlier_rtt_multiplier <= 0.0) {
        std::cerr << "--outlier-rtt-multiplier must be positive\n";
        std::exit(1);
    }
    if (cfg.causal_chain_fraction < 0.0 || cfg.causal_chain_fraction > 1.0) {
        std::cerr << "--causal-chain-fraction must be within [0, 1]\n";
        std::exit(1);
    }
    if (cfg.reorder_probability < 0.0 || cfg.reorder_probability > 1.0) {
        std::cerr << "--reorder-probability must be within [0, 1]\n";
        std::exit(1);
    }
    return cfg;
}

const char* event_type_name(chronos::EventType t) {
    switch (t) {
        case chronos::EventType::OrderSubmitted:   return "OrderSubmitted";
        case chronos::EventType::OrderAcknowledged:return "OrderAcknowledged";
        case chronos::EventType::OrderExecuted:    return "OrderExecuted";
        case chronos::EventType::QuoteUpdated:     return "QuoteUpdated";
        case chronos::EventType::TradePublished:   return "TradePublished";
        case chronos::EventType::Heartbeat:        return "Heartbeat";
    }
    return "Unknown";
}

// Seed derivation constants. Each stream (clock, event gaps, sync-link
// latency, event-delivery latency) gets an independent per-source seed, all
// derived from the single top-level --seed so the whole run stays
// reproducible from one number while no two streams ever share state.
constexpr std::uint64_t kClockSeedMul         = 6364136223846793005ULL;
constexpr std::uint64_t kEventGapSeedMul      = 2862933555777941757ULL;
constexpr std::uint64_t kSyncLatencySeedMul   = 0xBF58476D1CE4E5B9ULL;
constexpr std::uint64_t kEventLatencySeedMul  = 0x94D049BB133111EBULL;
constexpr std::uint64_t kCausalLinkSeedMul    = 0xD6E8FEB86659FD93ULL;

} // namespace

int main(int argc, char** argv) {
    const SimConfig cfg = parse_args(argc, argv);

    const int events_per_source = cfg.total_events / cfg.num_sources;
    const int remainder         = cfg.total_events % cfg.num_sources;

    // Simulation reference time: Unix epoch 2026-01-01 00:00:00 in nanoseconds.
    // Arbitrary but fixed so results are reproducible.
    constexpr chronos::NsTimestamp kReferenceTimeNs = 1'767'225'600LL * 1'000'000'000LL;

    const auto sync_interval_ns = static_cast<chronos::NsDuration>(cfg.sync_interval_us * 1'000.0);
    const auto base_latency_ns  = static_cast<chronos::NsDuration>(cfg.base_latency_us * 1'000.0);
    const auto asymmetry_ns     = static_cast<chronos::NsDuration>(cfg.latency_asymmetry_us * 1'000.0);
    const double jitter_std_ns  = cfg.jitter_us * 1'000.0;

    std::vector<chronos::MarketEvent> all_events;
    all_events.reserve(cfg.total_events);

    // Recorded at generation time (not recomputed later by scanning
    // source_id) so the Phase 6 async delivery pass below has a
    // source's exact slice within all_events regardless of whether any
    // later pass ever reorders events in place (it doesn't today, but this
    // removes the dependency on that continuing to hold).
    std::vector<std::size_t> source_offsets, source_counts;

    // One estimator of each kind per source, indexed by source id. Both are
    // fed every sync sample -- naive unfiltered, weighted with its own
    // sliding-window + outlier rejection -- so the report can compare them
    // directly ("naive vs. weighted" is this side-by-side, not a separate mode).
    std::vector<chronos::OffsetEstimator> estimators(static_cast<std::size_t>(cfg.num_sources));
    std::vector<chronos::DriftAwareOffsetEstimator> drift_estimators(
        static_cast<std::size_t>(cfg.num_sources),
        chronos::DriftAwareOffsetEstimator(chronos::DriftAwareOffsetEstimator::Config{
            .window_size            = static_cast<std::size_t>(cfg.drift_window),
            .outlier_rtt_multiplier = cfg.outlier_rtt_multiplier,
        }));

    // Each source gets a disjoint event_id range so ids stay globally unique
    // once events are pooled -- EventSource only guarantees uniqueness within
    // a single source, so the caller (here) owns assigning the ranges.
    std::uint64_t next_event_id_base = 0;

    std::size_t total_sync_samples = 0;

    for (int s = 0; s < cfg.num_sources; ++s) {
        // Spread offsets symmetrically: source 0 is most negative, last is most positive.
        // Avoids all sources having the same bias direction.
        const double offset_fraction = cfg.num_sources > 1
            ? (static_cast<double>(s) / (cfg.num_sources - 1) * 2.0 - 1.0)
            : 0.0;
        const chronos::NsDuration offset_ns =
            static_cast<chronos::NsDuration>(offset_fraction * cfg.max_offset_us * 1000.0);

        // Alternate drift sign so we get both fast and slow clocks.
        const double drift = (s % 2 == 0 ? 1.0 : -1.0) * cfg.drift_ppm;

        // Derive per-source, per-stream seeds deterministically from the global seed.
        // (kEventLatencySeedMul is reused below, at delivery time, for the
        // Phase 6 async pipeline's per-source LatencyModel -- not derived
        // here, since that model isn't constructed until every source has
        // been generated and pooled.)
        const auto su             = static_cast<std::uint64_t>(s);
        const std::uint64_t clock_seed         = cfg.seed * kClockSeedMul        + su;
        const std::uint64_t src_seed           = cfg.seed * kEventGapSeedMul     + su;
        const std::uint64_t sync_latency_seed  = cfg.seed * kSyncLatencySeedMul  + su;

        chronos::ClockConfig clock_cfg{
            .offset_ns    = offset_ns,
            .drift_ppm    = drift,
            .noise_std_ns = cfg.noise_std_ns,
            .seed         = clock_seed,
        };
        const int count = events_per_source + (s < remainder ? 1 : 0);
        chronos::EventSourceConfig src_cfg{
            .start_time_ns       = kReferenceTimeNs,
            .mean_inter_event_ns = static_cast<chronos::NsDuration>(cfg.inter_event_us * 1'000.0),
            .seed                = src_seed,
            .starting_event_id   = next_event_id_base,
        };

        chronos::EventSource source(
            static_cast<chronos::SourceId>(s),
            clock_cfg, src_cfg,
            kReferenceTimeNs);

        auto evts = source.generate(static_cast<std::size_t>(count));
        next_event_id_base += static_cast<std::uint64_t>(count);

        // receive_time is populated below by the Phase 6 async delivery
        // pipeline, once all sources' events are pooled -- delivery must
        // share one scheduler/collector across every source so cross-source
        // interleaving on a single collector (the whole point of doing this
        // asynchronously) is actually possible.

        chronos::NsTimestamp max_true_time = kReferenceTimeNs;
        for (const auto& e : evts) max_true_time = std::max(max_true_time, e.true_time);

        // Run the sync schedule over a window padded well past the last
        // event: OffsetEstimator looks samples up by an event's
        // source_local_time, a different clock than the collector-time keys
        // sync samples are stored under, so the two can disagree by up to
        // roughly the clock offset. Offset is ~constant between samples (only
        // drift changes it), so resolving to a neighboring sample doesn't
        // hurt correction accuracy -- this padding just guarantees a sample
        // exists to find at all.
        const chronos::NsDuration sync_pad_ns =
            sync_interval_ns * 4 + static_cast<chronos::NsDuration>(cfg.max_offset_us * 1000.0) * 2;

        chronos::ClockConfig sync_clock_cfg = clock_cfg; // same config; independent Sync noise channel
        chronos::ClockModel sync_clock(static_cast<chronos::SourceId>(s), sync_clock_cfg, kReferenceTimeNs);

        chronos::LatencyConfig sync_latency_cfg{
            .base_ns           = base_latency_ns,
            .asymmetry_ns      = asymmetry_ns,
            .jitter_std_ns     = jitter_std_ns,
            .spike_probability = cfg.spike_probability,
            .spike_multiplier  = cfg.spike_multiplier,
            .seed              = sync_latency_seed,
        };
        chronos::LatencyModel sync_latency(sync_latency_cfg);

        const auto samples = chronos::run_sync_schedule(
            static_cast<chronos::SourceId>(s), kReferenceTimeNs, max_true_time + sync_pad_ns,
            sync_interval_ns, sync_clock, sync_latency);

        for (const auto& sample : samples) {
            estimators[static_cast<std::size_t>(s)].add_sample(sample);
            drift_estimators[static_cast<std::size_t>(s)].add_sample(sample);
        }
        total_sync_samples += samples.size();

        source_offsets.push_back(all_events.size());
        source_counts.push_back(evts.size());
        for (auto& e : evts) all_events.push_back(std::move(e));
    }

    // Phase 5: assign cross-source causal chains over the pooled event
    // list. This mutates only event_type/parent_event_id, never a
    // timestamp -- every metric below is a function of timestamps alone,
    // so this call cannot perturb the Phase 3/4 numbers (verified in
    // results/phase5_baseline.txt against results/phase4_baseline.txt).
    const auto causal_link_stats = chronos::assign_causal_chains(
        all_events,
        chronos::CausalLinkConfig{
            .chain_fraction = cfg.causal_chain_fraction,
            .seed           = cfg.seed * kCausalLinkSeedMul,
        });

    // Phase 6: async delivery over a virtual-time Asio event loop. One
    // shared scheduler and one shared collector across ALL sources -- that
    // sharing is the point: cross-source interleaving on a single collector
    // is what per-source-isolated delivery could never produce. Placed
    // after causal linking (so the collector sees final event_type) and
    // before local_sorted/corrected_events are built (so the stamped
    // receive_time propagates into every downstream copy and the CSV).
    boost::asio::io_context io{1}; // concurrency hint 1: strictly single-threaded
    chronos::VirtualTimeScheduler scheduler(io);
    chronos::AsyncCollector collector(std::span<chronos::MarketEvent>{all_events});

    for (int s = 0; s < cfg.num_sources; ++s) {
        chronos::LatencyConfig delivery_cfg{
            .base_ns             = base_latency_ns,
            .asymmetry_ns        = asymmetry_ns,
            .jitter_std_ns       = jitter_std_ns,
            .spike_probability   = cfg.spike_probability,
            .spike_multiplier    = cfg.spike_multiplier,
            .reorder_probability = cfg.reorder_probability,
            // Reuses the exact seed stream the old synchronous fill used --
            // with --reorder-probability 0, every receive_time below is
            // therefore bit-identical to what a Phase 5 run would have
            // produced with the naive fill, since it's the same generator
            // consumed in the same order. Only delivery ORDER is new.
            .seed = cfg.seed * kEventLatencySeedMul + static_cast<std::uint64_t>(s),
        };
        chronos::LatencyModel delivery(delivery_cfg);
        chronos::schedule_source_delivery(
            scheduler, collector, delivery,
            std::span<const chronos::MarketEvent>{all_events}.subspan(
                source_offsets[static_cast<std::size_t>(s)], source_counts[static_cast<std::size_t>(s)]),
            source_offsets[static_cast<std::size_t>(s)],
            cfg.reorder_probability > 0.0);
    }

    const auto async_start   = std::chrono::steady_clock::now();
    const std::size_t delivered = scheduler.run();
    const auto async_elapsed = std::chrono::steady_clock::now() - async_start;
    // Wall-clock timing goes to stderr, never stdout: a real-time number on
    // stdout would break the byte-for-byte reproducibility diff every
    // committed results/*.txt file depends on.
    std::cerr << "Async delivery: " << delivered << " events in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(async_elapsed).count() << " ms\n";

    // Correction pass + accuracy measurement. Both use one canonical event
    // ordering -- sorted by source_local_time, matching Phase 1's raw-timeline
    // ordering exactly -- so raw and corrected accuracy sample identical
    // pairs and the raw number stays reproducible across phases; the delta
    // is attributable solely to correction, not to sampling variance from a
    // different input order. (The metric ignores order once computed exactly;
    // this only matters because pair *sampling* above the exact-pair ceiling
    // is index-based.)
    std::vector<chronos::MarketEvent> local_sorted = all_events;
    std::sort(local_sorted.begin(), local_sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.source_local_time < b.source_local_time;
              });

    std::vector<chronos::NsTimestamp> true_times, raw_times, naive_times, weighted_times, receive_times;
    true_times.reserve(local_sorted.size());
    raw_times.reserve(local_sorted.size());
    naive_times.reserve(local_sorted.size());
    weighted_times.reserve(local_sorted.size());
    receive_times.reserve(local_sorted.size());

    // Per-event correction error (|true_time - corrected_time|), collected
    // separately per estimator -- this is what CLAUDE.md's median/p95/p99
    // "offset estimation error" metrics are measured against, not raw
    // sync-sample noise: it reflects what actually reaches ordering accuracy.
    std::vector<double> naive_error_ns, weighted_error_ns;

    // Phase 4: weighted-corrected events carrying an uncertainty interval,
    // fed to the ambiguity classifier below. Events with no sync sample yet
    // get a zero-width interval at the uncorrected local time -- consistent
    // with the point-estimate fallback above, and such events are rare
    // (only the first few per source, before their first sync sample).
    std::vector<chronos::CorrectedEvent> corrected_events;
    corrected_events.reserve(local_sorted.size());

    std::size_t naive_uncorrected = 0, weighted_uncorrected = 0;
    for (const auto& e : local_sorted) {
        true_times.push_back(e.true_time);
        raw_times.push_back(e.source_local_time);
        receive_times.push_back(e.receive_time);

        const auto src_idx = static_cast<std::size_t>(static_cast<std::uint32_t>(e.source_id));

        const auto naive_offset = estimators[src_idx].estimate_at(e.source_local_time);
        if (naive_offset) {
            const chronos::NsTimestamp corrected = e.source_local_time - *naive_offset;
            naive_times.push_back(corrected);
            naive_error_ns.push_back(std::abs(static_cast<double>(e.true_time - corrected)));
        } else {
            naive_times.push_back(e.source_local_time); // no sync sample yet -- leave uncorrected
            ++naive_uncorrected;
        }

        const auto weighted_est = drift_estimators[src_idx].estimate_with_uncertainty_at(e.source_local_time);
        chronos::CorrectedEvent ce;
        ce.original = e;
        if (weighted_est) {
            const chronos::NsTimestamp corrected = e.source_local_time - weighted_est->offset;
            weighted_times.push_back(corrected);
            weighted_error_ns.push_back(std::abs(static_cast<double>(e.true_time - corrected)));
            ce.corrected_time = corrected;
            ce.lower_bound     = corrected - weighted_est->uncertainty;
            ce.upper_bound     = corrected + weighted_est->uncertainty;
        } else {
            weighted_times.push_back(e.source_local_time); // no sync sample yet -- leave uncorrected
            ++weighted_uncorrected;
            ce.corrected_time = e.source_local_time;
            ce.lower_bound     = e.source_local_time;
            ce.upper_bound     = e.source_local_time;
        }
        corrected_events.push_back(ce);
    }

    std::size_t total_rejected = 0;
    for (const auto& de : drift_estimators) total_rejected += de.rejected_count();

    const chronos::AccuracyResult raw_result =
        chronos::pairwise_ordering_accuracy(true_times, raw_times, cfg.seed);
    const chronos::AccuracyResult naive_result =
        chronos::pairwise_ordering_accuracy(true_times, naive_times, cfg.seed);
    const chronos::AccuracyResult weighted_result =
        chronos::pairwise_ordering_accuracy(true_times, weighted_times, cfg.seed);

    // Phase 6: ordering accuracy if events were sorted purely by
    // collector-arrival time. A different failure mode than raw_result: the
    // collector clock is one clock, immune to per-source clock skew, but
    // arrival order is polluted by network latency jitter instead.
    const chronos::AccuracyResult arrival_result =
        chronos::pairwise_ordering_accuracy(true_times, receive_times, cfg.seed);

    const chronos::AmbiguityResult ambiguity_result =
        chronos::classify_pairs(corrected_events, cfg.seed);

    // Phase 5: causal graph, violation detection, and interval narrowing.
    // `corrected_events` (and the parallel true_times/raw_times/weighted_times
    // vectors) share one index space, built in the same loop above, so the
    // graph and every check below index consistently across all of them.
    const chronos::CausalGraph causal_graph = chronos::CausalGraph::build(corrected_events);

    const auto ground_truth_violations = chronos::check_point_violations(causal_graph, true_times);
    const auto raw_violations          = chronos::check_point_violations(causal_graph, raw_times);
    const auto corrected_violations    = chronos::check_point_violations(causal_graph, weighted_times);
    const auto interval_violations     = chronos::check_interval_violations(causal_graph, corrected_events);

    std::vector<chronos::CorrectedEvent> narrowed_events = corrected_events;
    const auto narrowing_result = chronos::narrow_intervals(causal_graph, narrowed_events);

    std::vector<chronos::NsTimestamp> narrowed_times;
    narrowed_times.reserve(narrowed_events.size());
    for (const auto& e : narrowed_events) narrowed_times.push_back(e.corrected_time);
    const auto narrowed_corrected_violations = chronos::check_point_violations(causal_graph, narrowed_times);

    const chronos::AccuracyResult narrowed_result =
        chronos::pairwise_ordering_accuracy(true_times, narrowed_times, cfg.seed);
    const chronos::AmbiguityResult narrowed_ambiguity_result =
        chronos::classify_pairs(narrowed_events, cfg.seed);
    const chronos::CausalPairResult causal_pair_result =
        chronos::evaluate_causal_pairs(causal_graph, corrected_events, narrowed_events);

    // Ground-truth order, for the display table and CSV below.
    std::vector<chronos::MarketEvent> true_sorted = all_events;
    std::sort(true_sorted.begin(), true_sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.true_time < b.true_time;
              });

    // Print report.
    std::cout << "\nChronos Phase 6 — Simulation Report\n";
    std::cout << "-------------------------------------\n";
    std::cout << "Sources:                 " << cfg.num_sources   << "\n";
    std::cout << "Events generated:        " << all_events.size() << "\n";
    std::cout << "Seed:                    " << cfg.seed          << "\n";
    std::cout << "Drift (PPM):             ±" << cfg.drift_ppm    << "\n";
    std::cout << "Max offset (us):         ±" << cfg.max_offset_us<< "\n";
    std::cout << "Noise std (ns):          " << cfg.noise_std_ns  << "\n";
    std::cout << "Sync interval (us):      " << cfg.sync_interval_us << "\n";
    std::cout << "Base latency (us):       " << cfg.base_latency_us  << "\n";
    std::cout << "Drift window (samples):  " << cfg.drift_window  << "\n";
    std::cout << "Outlier RTT multiplier:  " << cfg.outlier_rtt_multiplier << "\n";
    std::cout << "Causal chain fraction:   " << cfg.causal_chain_fraction  << "\n";
    std::cout << "Reorder probability:     " << cfg.reorder_probability    << "\n";
    std::cout << "\n";
    std::cout << std::fixed << std::setprecision(2);

    auto print_accuracy = [](const char* label, const chronos::AccuracyResult& r) {
        std::cout << label << r.accuracy * 100.0 << "%\n";
        if (r.sampled) {
            std::cout << "  (sampled " << r.pairs_considered << " of "
                       << r.total_pairs << " pairs -- exact computation would be O(n^2))\n";
        }
    };
    print_accuracy("Raw timestamp ordering accuracy:      ", raw_result);
    print_accuracy("Naive-corrected ordering accuracy:    ", naive_result);
    print_accuracy("Weighted-corrected ordering accuracy: ", weighted_result);
    print_accuracy("Arrival-order (receive_time) accuracy:", arrival_result);
    std::cout << "Improvement (raw -> naive):            "
               << (naive_result.accuracy - raw_result.accuracy) * 100.0 << " pp\n";
    std::cout << "Improvement (naive -> weighted):       "
               << (weighted_result.accuracy - naive_result.accuracy) * 100.0 << " pp\n";
    std::cout << "\n";
    std::cout << "Naive correction error    -- median / p95 / p99: "
               << chronos::percentile(naive_error_ns, 0.5)  << " / "
               << chronos::percentile(naive_error_ns, 0.95) << " / "
               << chronos::percentile(naive_error_ns, 0.99) << " ns\n";
    std::cout << "Weighted correction error -- median / p95 / p99: "
               << chronos::percentile(weighted_error_ns, 0.5)  << " / "
               << chronos::percentile(weighted_error_ns, 0.95) << " / "
               << chronos::percentile(weighted_error_ns, 0.99) << " ns\n";
    std::cout << "\n";
    std::cout << "\n";
    std::cout << "Confidence-interval classification (weighted-corrected events):\n";
    std::cout << "  Definite pairs:   " << ambiguity_result.definite_pairs << " ("
               << (ambiguity_result.pairs_considered > 0
                       ? 100.0 * static_cast<double>(ambiguity_result.definite_pairs) / static_cast<double>(ambiguity_result.pairs_considered)
                       : 0.0)
               << "%), correct: " << ambiguity_result.definite_correct << " ("
               << (ambiguity_result.definite_pairs > 0
                       ? 100.0 * static_cast<double>(ambiguity_result.definite_correct) / static_cast<double>(ambiguity_result.definite_pairs)
                       : 100.0)
               << "%)\n";
    std::cout << "  Ambiguous pairs:  " << ambiguity_result.ambiguous_pairs << " ("
               << (ambiguity_result.pairs_considered > 0
                       ? 100.0 * static_cast<double>(ambiguity_result.ambiguous_pairs) / static_cast<double>(ambiguity_result.pairs_considered)
                       : 0.0)
               << "%)\n";
    std::cout << "  Point-estimate errors: " << ambiguity_result.point_estimate_errors
               << ", caught by ambiguity flag: " << ambiguity_result.point_estimate_errors_flagged_ambiguous
               << " ("
               << (ambiguity_result.point_estimate_errors > 0
                       ? 100.0 * static_cast<double>(ambiguity_result.point_estimate_errors_flagged_ambiguous) / static_cast<double>(ambiguity_result.point_estimate_errors)
                       : 100.0)
               << "%)\n";
    if (ambiguity_result.sampled) {
        std::cout << "  (sampled " << ambiguity_result.pairs_considered << " of "
                   << ambiguity_result.total_pairs << " pairs -- exact computation would be O(n^2))\n";
    }
    std::cout << "\n";
    std::cout << "Causal graph: " << causal_link_stats.chains_created << " chains, "
               << causal_link_stats.events_linked << " events linked, " << causal_graph.edges().size()
               << " edges (" << causal_link_stats.abandoned_chains << " abandoned)\n";
    std::cout << "  Ground-truth violations: " << ground_truth_violations.violations
               << " (should always be 0 -- an invariant of assign_causal_chains)\n";
    std::cout << "  Raw timeline violations:       " << raw_violations.violations << " / "
               << raw_violations.edges_checked << " edges\n";
    std::cout << "  Corrected-point violations:    " << corrected_violations.violations << " / "
               << corrected_violations.edges_checked << " edges\n";
    std::cout << "  Corrected-interval violations: " << interval_violations.violations << " / "
               << interval_violations.edges_checked << " edges\n";
    if (!interval_violations.examples.empty()) {
        std::cout << "  Example impossible-timeline edge (raw-timeline parent -> child, local_time ns):\n";
        const auto& ex = interval_violations.examples.front();
        std::cout << "    parent event " << static_cast<std::uint64_t>(corrected_events[ex.parent].original.event_id)
                   << " local=" << corrected_events[ex.parent].original.source_local_time
                   << "  ->  child event " << static_cast<std::uint64_t>(corrected_events[ex.child].original.event_id)
                   << " local=" << corrected_events[ex.child].original.source_local_time << "\n";
    }
    std::cout << "\n";
    if (narrowing_result) {
        std::cout << "Causal interval narrowing: " << narrowing_result->nodes_narrowed << " nodes narrowed, "
                   << narrowing_result->inconsistent_components << " inconsistent components ("
                   << narrowing_result->nodes_reverted << " nodes reverted)\n";
        print_accuracy("Weighted + causal ordering accuracy:  ", narrowed_result);
        std::cout << "  Corrected-interval violations after narrowing: " << narrowed_corrected_violations.violations
                   << " / " << narrowed_corrected_violations.edges_checked << " edges (point check)\n";
        std::cout << "\n";
        std::cout << "Ambiguity before vs after causal narrowing (global, sampled pairs):\n";
        std::cout << "  Ambiguous pairs: " << ambiguity_result.ambiguous_pairs << " -> "
                   << narrowed_ambiguity_result.ambiguous_pairs << "\n";
        std::cout << "  Definite pairs:  " << ambiguity_result.definite_pairs << " -> "
                   << narrowed_ambiguity_result.definite_pairs << " (correct: " << ambiguity_result.definite_correct
                   << " -> " << narrowed_ambiguity_result.definite_correct << ")\n";
        std::cout << "\n";
        std::cout << "Ambiguity on causally-related pairs only (exact, not sampled -- see README on why\n";
        std::cout << "global sampling dilutes this signal):\n";
        std::cout << "  Pairs evaluated: " << causal_pair_result.pairs << "\n";
        std::cout << "  Ambiguous: " << causal_pair_result.ambiguous_before << " -> "
                   << causal_pair_result.ambiguous_after << "\n";
        std::cout << "  Definite:  " << causal_pair_result.definite_before << " -> "
                   << causal_pair_result.definite_after << " (correct: " << causal_pair_result.definite_correct_before
                   << " -> " << causal_pair_result.definite_correct_after << ")\n";
    } else {
        std::cout << "Causal interval narrowing: skipped (graph contains a cycle)\n";
    }
    std::cout << "\n";
    {
        const auto& realized = collector.realized_latency_ns();
        const auto  ds       = collector.stats();
        std::cout << "Async delivery (virtual-time Asio pipeline):\n";
        std::cout << "  Events delivered:            " << delivered << " / " << all_events.size() << "\n";
        std::cout << "  Realized delivery latency -- median / p95 / p99: "
                   << chronos::percentile(realized, 0.5)  << " / "
                   << chronos::percentile(realized, 0.95) << " / "
                   << chronos::percentile(realized, 0.99) << " ns\n";
        std::cout << "  Same-source arrival inversions: " << ds.same_source_inversions << " / "
                   << ds.same_source_pairs << " adjacent pairs ("
                   << (ds.same_source_pairs > 0
                           ? 100.0 * static_cast<double>(ds.same_source_inversions) / static_cast<double>(ds.same_source_pairs)
                           : 0.0)
                   << "%)\n";
        std::cout << "  Reorder probability:         " << cfg.reorder_probability << "\n";
    }
    std::cout << "\n";
    std::cout << "Sync samples: " << total_sync_samples << " total, " << total_rejected
               << " rejected as RTT outliers ("
               << (total_sync_samples > 0 ? 100.0 * static_cast<double>(total_rejected) / static_cast<double>(total_sync_samples) : 0.0)
               << "%)\n";
    std::cout << "Events corrected: " << (local_sorted.size() - naive_uncorrected) << " naive ("
               << naive_uncorrected << " uncorrected), " << (local_sorted.size() - weighted_uncorrected)
               << " weighted (" << weighted_uncorrected << " uncorrected)\n";
    std::cout << "\n";
    std::cout << "First 10 events by true order vs raw order:\n";
    std::cout << std::setw(6) << "Rank"
              << std::setw(9) << "SrcID"
              << std::setw(20) << "true_time (ns)"
              << std::setw(20) << "local_time (ns)"
              << std::setw(14) << "offset (ns)"
              << "\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(10, true_sorted.size()); ++i) {
        const auto& e = true_sorted[i];
        std::cout << std::setw(6)  << i
                  << std::setw(9)  << static_cast<std::uint32_t>(e.source_id)
                  << std::setw(20) << e.true_time
                  << std::setw(20) << e.source_local_time
                  << std::setw(14) << (e.source_local_time - e.true_time)
                  << "\n";
    }

    // Optional CSV output.
    if (!cfg.output_path.empty()) {
        std::ofstream out(cfg.output_path);
        if (!out) {
            std::cerr << "Could not open output file: " << cfg.output_path << "\n";
            return 1;
        }
        out << "event_id,source_id,event_type,true_time_ns,local_time_ns,offset_ns,parent_event_id,receive_time_ns\n";
        for (const auto& e : true_sorted) {
            out << static_cast<std::uint64_t>(e.event_id)   << ","
                << static_cast<std::uint32_t>(e.source_id)  << ","
                << event_type_name(e.event_type)             << ","
                << e.true_time                               << ","
                << e.source_local_time                       << ","
                << (e.source_local_time - e.true_time)       << ",";
            if (e.parent_event_id) out << static_cast<std::uint64_t>(*e.parent_event_id);
            out << "," << e.receive_time << "\n";
        }
        std::cout << "\nEvents written to: " << cfg.output_path << "\n";
    }

    return 0;
}
