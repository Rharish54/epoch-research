// Benchmarks for timeline reconstruction (causal graph, interval narrowing,
// ambiguity classification) and end-to-end simulation+reconstruction.
//
// This file PORTS src/main.cpp's pipeline into staged functions rather than
// refactoring main.cpp into a shared library entry point -- some setup-code
// duplication is accepted scope here, consistent with this project's
// documented anti-premature-abstraction stance (a shared `run_simulation()`
// is listed as a Future Improvement once a second real caller exists).
//
// CRITICAL: the corpus this file builds at {sources=16, events=5000, seed=42}
// MUST reproduce the numbers already committed in results/phase5_baseline.txt
// exactly (71.36% raw accuracy, 92.46% weighted-corrected accuracy, 997
// causal edges, 487 raw-timeline violations) -- see the
// CorpusMatchesCommittedBaseline sanity check below, which is not a
// benchmark but a plain assertion run once at static-init time. If it ever
// fails, every benchmark number in this file is measuring a different
// system than the one the README describes, and must not be trusted until
// fixed.
//
// No BENCHMARK_MAIN() -- links against benchmark::benchmark_main.
// Release-only per CLAUDE.md's Benchmarking Rules.

#include "clock/clock_model.hpp"
#include "clock/drift_estimator.hpp"
#include "clock/offset_estimator.hpp"
#include "metrics/accuracy.hpp"
#include "metrics/ambiguity.hpp"
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
#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <span>
#include <vector>

using namespace chronos;

namespace {

// ---- Seed constants, copied verbatim from src/main.cpp ----
constexpr std::uint64_t kClockSeedMul        = 6364136223846793005ULL;
constexpr std::uint64_t kEventGapSeedMul     = 2862933555777941757ULL;
constexpr std::uint64_t kSyncLatencySeedMul  = 0xBF58476D1CE4E5B9ULL;
constexpr std::uint64_t kEventLatencySeedMul = 0x94D049BB133111EBULL;
constexpr std::uint64_t kCausalLinkSeedMul   = 0xD6E8FEB86659FD93ULL;
constexpr NsTimestamp kReferenceTimeNs       = 1'767'225'600LL * 1'000'000'000LL;

// Mirrors SimConfig's CLI defaults (src/main.cpp:35-54) -- only the fields
// the pipeline stages below actually consume.
struct BenchConfig {
    int            num_sources           = 16;
    int            total_events          = 5000;
    std::uint64_t  seed                  = 42;
    double         drift_ppm             = 300.0;
    double         max_offset_us         = 2000.0;
    double         noise_std_ns          = 50.0;
    double         inter_event_us        = 10.0;
    double         sync_interval_us      = 200.0;
    double         base_latency_us       = 100.0;
    double         latency_asymmetry_us  = 0.0;
    double         jitter_us             = 50.0;
    double         spike_probability     = 0.0;
    double         spike_multiplier      = 10.0;
    int            drift_window          = 10;
    double         outlier_rtt_multiplier = 3.0;
    double         causal_chain_fraction = 0.3;
    double         reorder_probability   = 0.0;
};

struct Corpus {
    std::vector<MarketEvent> all_events;
    std::vector<std::size_t> source_offsets, source_counts;
    std::vector<OffsetEstimator>           estimators;
    std::vector<DriftAwareOffsetEstimator> drift_estimators;

    std::vector<MarketEvent>  local_sorted;
    std::vector<NsTimestamp>  true_times, raw_times, weighted_times, receive_times;
    std::vector<CorrectedEvent> corrected;

    CausalLinkStats     causal_link_stats{};
    std::size_t         delivered = 0;
};

// Stage 1: generate every source's events and run its sync schedule.
// Ports src/main.cpp:241-372.
void generate_and_sync(const BenchConfig& cfg, Corpus& c) {
    const int events_per_source = cfg.total_events / cfg.num_sources;
    const int remainder         = cfg.total_events % cfg.num_sources;

    const auto sync_interval_ns = static_cast<NsDuration>(cfg.sync_interval_us * 1'000.0);
    const auto base_latency_ns  = static_cast<NsDuration>(cfg.base_latency_us * 1'000.0);
    const auto asymmetry_ns     = static_cast<NsDuration>(cfg.latency_asymmetry_us * 1'000.0);
    const double jitter_std_ns  = cfg.jitter_us * 1'000.0;

    c.all_events.reserve(static_cast<std::size_t>(cfg.total_events));
    c.estimators.assign(static_cast<std::size_t>(cfg.num_sources), OffsetEstimator{});
    c.drift_estimators.assign(static_cast<std::size_t>(cfg.num_sources),
        DriftAwareOffsetEstimator(DriftAwareOffsetEstimator::Config{
            .window_size = static_cast<std::size_t>(cfg.drift_window), .outlier_rtt_multiplier = cfg.outlier_rtt_multiplier}));

    std::uint64_t next_event_id_base = 0;

    for (int s = 0; s < cfg.num_sources; ++s) {
        const double offset_fraction =
            cfg.num_sources > 1 ? (static_cast<double>(s) / (cfg.num_sources - 1) * 2.0 - 1.0) : 0.0;
        const auto offset_ns = static_cast<NsDuration>(offset_fraction * cfg.max_offset_us * 1000.0);
        const double drift   = (s % 2 == 0 ? 1.0 : -1.0) * cfg.drift_ppm;

        const auto su                          = static_cast<std::uint64_t>(s);
        const std::uint64_t clock_seed         = cfg.seed * kClockSeedMul        + su;
        const std::uint64_t src_seed           = cfg.seed * kEventGapSeedMul     + su;
        const std::uint64_t sync_latency_seed  = cfg.seed * kSyncLatencySeedMul  + su;

        ClockConfig clock_cfg{.offset_ns = offset_ns, .drift_ppm = drift, .noise_std_ns = cfg.noise_std_ns, .seed = clock_seed};
        const int count = events_per_source + (s < remainder ? 1 : 0);
        EventSourceConfig src_cfg{
            .start_time_ns = kReferenceTimeNs,
            .mean_inter_event_ns = static_cast<NsDuration>(cfg.inter_event_us * 1'000.0),
            .seed = src_seed,
            .starting_event_id = next_event_id_base,
        };

        EventSource source(static_cast<SourceId>(s), clock_cfg, src_cfg, kReferenceTimeNs);
        auto evts = source.generate(static_cast<std::size_t>(count));
        next_event_id_base += static_cast<std::uint64_t>(count);

        NsTimestamp max_true_time = kReferenceTimeNs;
        for (const auto& e : evts) max_true_time = std::max(max_true_time, e.true_time);

        const NsDuration sync_pad_ns = sync_interval_ns * 4 + static_cast<NsDuration>(cfg.max_offset_us * 1000.0) * 2;

        ClockConfig sync_clock_cfg = clock_cfg;
        ClockModel sync_clock(static_cast<SourceId>(s), sync_clock_cfg, kReferenceTimeNs);

        LatencyConfig sync_latency_cfg{
            .base_ns = base_latency_ns, .asymmetry_ns = asymmetry_ns, .jitter_std_ns = jitter_std_ns,
            .spike_probability = cfg.spike_probability, .spike_multiplier = cfg.spike_multiplier, .seed = sync_latency_seed,
        };
        LatencyModel sync_latency(sync_latency_cfg);

        const auto samples = run_sync_schedule(static_cast<SourceId>(s), kReferenceTimeNs, max_true_time + sync_pad_ns,
            sync_interval_ns, sync_clock, sync_latency);

        for (const auto& sample : samples) {
            c.estimators[static_cast<std::size_t>(s)].add_sample(sample);
            c.drift_estimators[static_cast<std::size_t>(s)].add_sample(sample);
        }

        c.source_offsets.push_back(c.all_events.size());
        c.source_counts.push_back(evts.size());
        for (auto& e : evts) c.all_events.push_back(std::move(e));
    }
}

// Stage 2: cross-source causal chains. Ports src/main.cpp:379-384.
void link_causal_chains(const BenchConfig& cfg, Corpus& c) {
    c.causal_link_stats = assign_causal_chains(
        c.all_events, CausalLinkConfig{.chain_fraction = cfg.causal_chain_fraction, .seed = cfg.seed * kCausalLinkSeedMul});
}

// Stage 3: async delivery over a virtual-time Asio event loop. Ports
// src/main.cpp:393-422 (minus the stderr timing print).
void deliver_async(const BenchConfig& cfg, Corpus& c) {
    const auto base_latency_ns = static_cast<NsDuration>(cfg.base_latency_us * 1'000.0);
    const auto asymmetry_ns    = static_cast<NsDuration>(cfg.latency_asymmetry_us * 1'000.0);
    const double jitter_std_ns = cfg.jitter_us * 1'000.0;

    boost::asio::io_context io{1};
    VirtualTimeScheduler scheduler(io);
    AsyncCollector collector(std::span<MarketEvent>{c.all_events});

    for (int s = 0; s < cfg.num_sources; ++s) {
        LatencyConfig delivery_cfg{
            .base_ns = base_latency_ns, .asymmetry_ns = asymmetry_ns, .jitter_std_ns = jitter_std_ns,
            .spike_probability = cfg.spike_probability, .spike_multiplier = cfg.spike_multiplier,
            .reorder_probability = cfg.reorder_probability,
            .seed = cfg.seed * kEventLatencySeedMul + static_cast<std::uint64_t>(s),
        };
        LatencyModel delivery(delivery_cfg);
        schedule_source_delivery(scheduler, collector, delivery,
            std::span<const MarketEvent>{c.all_events}.subspan(
                c.source_offsets[static_cast<std::size_t>(s)], c.source_counts[static_cast<std::size_t>(s)]),
            c.source_offsets[static_cast<std::size_t>(s)], cfg.reorder_probability > 0.0);
    }
    c.delivered = scheduler.run();
}

// Stage 4: sort + drift-correct every event into a CorrectedEvent interval.
// Ports src/main.cpp:438-501.
void correct_and_sort(Corpus& c) {
    c.local_sorted = c.all_events;
    std::sort(c.local_sorted.begin(), c.local_sorted.end(),
        [](const auto& a, const auto& b) { return a.source_local_time < b.source_local_time; });

    c.true_times.reserve(c.local_sorted.size());
    c.raw_times.reserve(c.local_sorted.size());
    c.weighted_times.reserve(c.local_sorted.size());
    c.receive_times.reserve(c.local_sorted.size());
    c.corrected.reserve(c.local_sorted.size());

    for (const auto& e : c.local_sorted) {
        c.true_times.push_back(e.true_time);
        c.raw_times.push_back(e.source_local_time);
        c.receive_times.push_back(e.receive_time);

        const auto src_idx = static_cast<std::size_t>(static_cast<std::uint32_t>(e.source_id));
        const auto weighted_est = c.drift_estimators[src_idx].estimate_with_uncertainty_at(e.source_local_time);

        CorrectedEvent ce;
        ce.original = e;
        if (weighted_est) {
            const NsTimestamp corrected = e.source_local_time - weighted_est->offset;
            c.weighted_times.push_back(corrected);
            ce.corrected_time = corrected;
            ce.lower_bound     = corrected - weighted_est->uncertainty;
            ce.upper_bound     = corrected + weighted_est->uncertainty;
        } else {
            c.weighted_times.push_back(e.source_local_time);
            ce.corrected_time = e.source_local_time;
            ce.lower_bound     = e.source_local_time;
            ce.upper_bound     = e.source_local_time;
        }
        c.corrected.push_back(ce);
    }
}

// Stage 5: causal graph. Ports src/main.cpp:527.
CausalGraph build_graph(const Corpus& c) { return CausalGraph::build(c.corrected); }

// Builds a full corpus through stage 4 (correction), NOT yet graphed.
Corpus build_corpus(const BenchConfig& cfg) {
    Corpus c;
    generate_and_sync(cfg, c);
    link_causal_chains(cfg, c);
    deliver_async(cfg, c);
    correct_and_sort(c);
    return c;
}

// Memoized, read-only corpora for benchmarks that only read the result.
// Google Benchmark runs single-threaded here (no ->Threads() used anywhere
// in this file), so a plain function-local static needs no locking.
const Corpus& cached_corpus(int sources, int events) {
    static std::map<std::pair<int, int>, Corpus> cache;
    const auto key = std::make_pair(sources, events);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    BenchConfig cfg;
    cfg.num_sources  = sources;
    cfg.total_events = events;
    return cache.emplace(key, build_corpus(cfg)).first->second;
}

// ---- Corpus validation: not a benchmark, a one-time sanity assertion ----
// Runs once at static-init time. If this ever fails, the port has diverged
// from src/main.cpp and no benchmark number below can be trusted until
// fixed -- see the file header comment.
struct CorpusValidation {
    CorpusValidation() {
        const auto& c = cached_corpus(16, 5000);
        const auto graph = CausalGraph::build(c.corrected);

        const auto raw_acc = pairwise_ordering_accuracy(c.true_times, c.raw_times, 42);
        const auto weighted_acc = pairwise_ordering_accuracy(c.true_times, c.weighted_times, 42);
        const auto raw_violations = check_point_violations(graph, c.raw_times);

        auto near = [](double a, double b) { return std::abs(a - b) < 0.01; };
        const bool ok = near(raw_acc.accuracy * 100.0, 71.36) && near(weighted_acc.accuracy * 100.0, 92.46) &&
                        graph.edges().size() == 997 && raw_violations.violations == 487;
        if (!ok) {
            std::fprintf(stderr,
                "FATAL: reconstruction_benchmark's ported corpus does not match the committed "
                "results/phase5_baseline.txt baseline (raw=%.2f%% weighted=%.2f%% edges=%zu raw_violations=%zu -- "
                "expected raw=71.36%% weighted=92.46%% edges=997 raw_violations=487). "
                "The port has diverged from src/main.cpp; fix before trusting any benchmark below.\n",
                raw_acc.accuracy * 100.0, weighted_acc.accuracy * 100.0, graph.edges().size(), raw_violations.violations);
            std::abort();
        }
    }
};
const CorpusValidation g_corpus_validation; // NOLINT -- runs once at static init, deliberately

} // namespace

// ==== Reconstruction-stage benchmarks (pre-built corpus, read-only) ====

static void BM_CausalGraph_Build(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    for (auto _ : state) benchmark::DoNotOptimize(CausalGraph::build(c.corrected));
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(c.corrected.size()));
}
BENCHMARK(BM_CausalGraph_Build)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

static void BM_CheckPointViolations(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    const auto graph = build_graph(c);
    for (auto _ : state) benchmark::DoNotOptimize(check_point_violations(graph, c.weighted_times));
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(graph.edges().size()));
}
BENCHMARK(BM_CheckPointViolations)->Arg(5000)->Arg(20000)->Unit(benchmark::kMicrosecond);

static void BM_CheckIntervalViolations(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    const auto graph = build_graph(c);
    for (auto _ : state) benchmark::DoNotOptimize(check_interval_violations(graph, c.corrected));
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(graph.edges().size()));
}
BENCHMARK(BM_CheckIntervalViolations)->Arg(5000)->Arg(20000)->Unit(benchmark::kMicrosecond);

static void BM_ClassifyPairs(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    for (auto _ : state) benchmark::DoNotOptimize(classify_pairs(c.corrected, 42));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ClassifyPairs)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

static void BM_PairwiseOrderingAccuracy(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    for (auto _ : state) benchmark::DoNotOptimize(pairwise_ordering_accuracy(c.true_times, c.weighted_times, 42));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PairwiseOrderingAccuracy)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

static void BM_EvaluateCausalPairs(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    const auto graph = build_graph(c);
    auto narrowed = c.corrected;
    narrow_intervals(graph, narrowed);
    for (auto _ : state) benchmark::DoNotOptimize(evaluate_causal_pairs(graph, c.corrected, narrowed));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EvaluateCausalPairs)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Mutates in place -- must copy the corpus fresh each iteration. Copying is
// paused out of the timed region; at n>=5000 this is roughly 1us/iteration
// of untimed overhead, negligible relative to what's actually measured.
static void BM_NarrowIntervals(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));
    const auto graph = build_graph(c);
    std::vector<CorrectedEvent> scratch;
    for (auto _ : state) {
        state.PauseTiming();
        scratch = c.corrected;
        state.ResumeTiming();
        benchmark::DoNotOptimize(narrow_intervals(graph, scratch));
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(c.corrected.size()));
}
BENCHMARK(BM_NarrowIntervals)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// The reconstruction-only pipeline over a pre-generated+delivered corpus:
// correct_and_sort + build_graph + narrow_intervals. This benchmark's
// items_per_second is what fills the resume bullet's [Z] events/sec figure
// (see README's Results Summary).
static void BM_Reconstruct_Pipeline(benchmark::State& state) {
    const auto events = static_cast<int>(state.range(0));
    BenchConfig cfg;
    cfg.total_events = events;
    Corpus base;
    generate_and_sync(cfg, base);
    link_causal_chains(cfg, base);
    deliver_async(cfg, base);

    for (auto _ : state) {
        state.PauseTiming();
        Corpus c;
        c.all_events        = base.all_events;
        c.estimators         = base.estimators;
        c.drift_estimators    = base.drift_estimators;
        state.ResumeTiming();

        correct_and_sort(c);
        const auto graph = build_graph(c);
        auto narrowed = c.corrected;
        narrow_intervals(graph, narrowed);
        benchmark::DoNotOptimize(narrowed);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(events));
}
BENCHMARK(BM_Reconstruct_Pipeline)->Arg(5000)->Arg(20000)->Arg(50000)->Unit(benchmark::kMillisecond);

// ==== End-to-end benchmarks (regenerate the simulation every iteration) ====

static void BM_EndToEnd_Full(benchmark::State& state) {
    BenchConfig cfg;
    cfg.num_sources  = static_cast<int>(state.range(0));
    cfg.total_events = static_cast<int>(state.range(1));
    for (auto _ : state) {
        Corpus c = build_corpus(cfg);
        const auto graph = build_graph(c);
        auto narrowed = c.corrected;
        narrow_intervals(graph, narrowed);
        benchmark::DoNotOptimize(narrowed);
    }
    state.SetItemsProcessed(state.iterations() * state.range(1));
}
BENCHMARK(BM_EndToEnd_Full)->ArgsProduct({{4, 16, 64}, {5000, 50000}})->Unit(benchmark::kMillisecond);

static void BM_EndToEnd_GenerateAndSync(benchmark::State& state) {
    BenchConfig cfg;
    cfg.num_sources  = static_cast<int>(state.range(0));
    cfg.total_events = static_cast<int>(state.range(1));
    for (auto _ : state) {
        Corpus c;
        generate_and_sync(cfg, c);
        benchmark::DoNotOptimize(c.all_events);
    }
    state.SetItemsProcessed(state.iterations() * state.range(1));
}
BENCHMARK(BM_EndToEnd_GenerateAndSync)->ArgsProduct({{4, 16, 64}, {5000, 50000}})->Unit(benchmark::kMillisecond);

// Scheduler+collector only, over a pre-generated+linked corpus copy --
// mirrors the Phase 6 source-count sweep (results/phase6_sources_*.txt) but
// with real statistics instead of millisecond-granularity wall-clock reads.
static void BM_EndToEnd_AsyncDelivery(benchmark::State& state) {
    BenchConfig cfg;
    cfg.num_sources  = static_cast<int>(state.range(0));
    cfg.total_events = static_cast<int>(state.range(1));

    Corpus base;
    generate_and_sync(cfg, base);
    link_causal_chains(cfg, base);

    for (auto _ : state) {
        state.PauseTiming();
        Corpus c;
        c.all_events      = base.all_events;
        c.source_offsets  = base.source_offsets;
        c.source_counts   = base.source_counts;
        state.ResumeTiming();

        deliver_async(cfg, c);
        benchmark::DoNotOptimize(c.delivered);
    }
    state.SetItemsProcessed(state.iterations() * state.range(1));
}
BENCHMARK(BM_EndToEnd_AsyncDelivery)->ArgsProduct({{1, 4, 16, 64}, {20000}})->Unit(benchmark::kMillisecond);

// ==== Per-event correction latency percentiles (p50/p95/p99) ====
//
// Google Benchmark reports mean/median/stddev ACROSS REPETITIONS natively,
// not per-item percentiles. Rather than a custom BenchmarkReporter, this
// times each event's estimate_with_uncertainty_at call individually and
// computes percentiles via chronos::percentile() -- the same utility
// main.cpp already uses for its offset-error and delivery-latency
// percentiles -- reported through state.counters so they land in the JSON
// output automatically.

#include "metrics/percentile.hpp"
#include <chrono>

static void BM_PerEventCorrectionLatency(benchmark::State& state) {
    const auto& c = cached_corpus(16, static_cast<int>(state.range(0)));

    for (auto _ : state) {
        std::vector<double> per_event_ns;
        per_event_ns.reserve(c.local_sorted.size());
        for (const auto& e : c.local_sorted) {
            const auto src_idx = static_cast<std::size_t>(static_cast<std::uint32_t>(e.source_id));
            const auto t0 = std::chrono::steady_clock::now();
            benchmark::DoNotOptimize(c.drift_estimators[src_idx].estimate_with_uncertainty_at(e.source_local_time));
            const auto t1 = std::chrono::steady_clock::now();
            per_event_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        state.counters["p50_ns"] = percentile(per_event_ns, 0.50);
        state.counters["p95_ns"] = percentile(per_event_ns, 0.95);
        state.counters["p99_ns"] = percentile(per_event_ns, 0.99);
    }
}
BENCHMARK(BM_PerEventCorrectionLatency)->Arg(50000)->Iterations(3)->Unit(benchmark::kMillisecond);

// Companion: bare-overhead of the two steady_clock::now() calls used above.
// Published alongside the percentiles above -- without this, the reported
// p50/p95/p99 are an unfalsifiable claim, since each includes ~2 clock
// reads of overhead. Treat BM_PerEventCorrectionLatency's counters as an
// upper bound including this overhead, not a pure measurement.
static void BM_TimerOverhead(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<double> samples;
        samples.reserve(1000);
        for (int i = 0; i < 1000; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto t1 = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        state.counters["p50_ns"] = percentile(samples, 0.50);
    }
}
BENCHMARK(BM_TimerOverhead)->Iterations(5)->Unit(benchmark::kMillisecond);
