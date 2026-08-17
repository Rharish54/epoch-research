// Benchmarks for clock-offset estimation: the naive OffsetEstimator and the
// weighted DriftAwareOffsetEstimator. No BENCHMARK_MAIN() -- this links
// against benchmark::benchmark_main (see CMakeLists.txt), which supplies it.
//
// Release-only per CLAUDE.md's Benchmarking Rules. Run with, e.g.:
//   ./build/estimator_benchmark --benchmark_min_warmup_time=0.1

#include "clock/drift_estimator.hpp"
#include "clock/offset_estimator.hpp"
#include "clock/sync_sample.hpp"
#include "core/types.hpp"

#include <benchmark/benchmark.h>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

using namespace chronos;

namespace {

// Same construction pattern as tests/drift_estimator_test.cpp's make_sample:
// send == receive == key, so the estimator's collector-time midpoint key
// lands exactly on `key`.
SyncSample make_sample(NsTimestamp key, NsDuration offset, NsDuration rtt) {
    SyncSample s;
    s.source_id              = static_cast<SourceId>(1);
    s.collector_send_time    = key;
    s.collector_receive_time = key;
    s.source_receive_time    = key;
    s.source_send_time       = key;
    s.estimated_offset       = offset;
    s.round_trip_delay       = rtt;
    return s;
}

constexpr NsTimestamp kRefTime = 1'000'000'000LL;

// Deterministic, seed-42 corpus: `key` spaced 200us apart (matching the CLI's
// --sync-interval-us default), `offset` following a 300ppm drift + 50ns
// gaussian noise (matching --drift-ppm/--noise-std-ns defaults), `rtt`
// nominally 200us +/- jitter, spiked 10x with probability `spike_fraction`
// (matching --spike-multiplier's default). add_sample requires non-decreasing
// key order, which this construction satisfies by construction.
struct SampleCorpus {
    std::vector<SyncSample> samples;
};

const SampleCorpus& make_corpus(std::size_t n, double spike_fraction) {
    static std::map<std::pair<std::size_t, int>, SampleCorpus> cache;
    const auto key = std::make_pair(n, static_cast<int>(spike_fraction * 100));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    std::mt19937_64 rng(42);
    std::normal_distribution<double> offset_noise(0.0, 50.0);
    std::normal_distribution<double> rtt_jitter(0.0, 20'000.0);
    std::bernoulli_distribution spike(spike_fraction);

    SampleCorpus corpus;
    corpus.samples.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const NsTimestamp t = kRefTime + static_cast<NsTimestamp>(i) * 200'000;
        const double drift_component = 300e-6 * static_cast<double>(t - kRefTime);
        const auto offset = static_cast<NsDuration>(1000 + drift_component + offset_noise(rng));
        auto rtt = static_cast<NsDuration>(200'000 + rtt_jitter(rng));
        if (rtt < 1) rtt = 1;
        if (spike(rng)) rtt *= 10;
        corpus.samples.push_back(make_sample(t, offset, rtt));
    }

    return cache.emplace(key, std::move(corpus)).first->second;
}

} // namespace

// ---- DriftAwareOffsetEstimator: ingest throughput ----

static void BM_DriftEstimator_AddSample(benchmark::State& state) {
    const auto n      = static_cast<std::size_t>(state.range(0));
    const auto window = static_cast<std::size_t>(state.range(1));
    const auto& corpus = make_corpus(n, 0.0);

    for (auto _ : state) {
        DriftAwareOffsetEstimator est{
            DriftAwareOffsetEstimator::Config{.window_size = window, .outlier_rtt_multiplier = 3.0}};
        for (const auto& s : corpus.samples) est.add_sample(s);
        benchmark::DoNotOptimize(est.accepted_count());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_DriftEstimator_AddSample)->ArgsProduct({{64, 512, 4096}, {5, 10, 25, 50}})->Unit(benchmark::kMicrosecond);

// ---- DriftAwareOffsetEstimator: ingest under outlier spikes ----

static void BM_DriftEstimator_AddSample_Spiky(benchmark::State& state) {
    const auto n              = static_cast<std::size_t>(state.range(0));
    const auto spike_fraction = static_cast<double>(state.range(1)) / 100.0;
    const auto& corpus = make_corpus(n, spike_fraction);

    for (auto _ : state) {
        DriftAwareOffsetEstimator est;
        for (const auto& s : corpus.samples) est.add_sample(s);
        benchmark::DoNotOptimize(est.rejected_count());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_DriftEstimator_AddSample_Spiky)->ArgsProduct({{4096}, {0, 5, 25}})->Unit(benchmark::kMicrosecond);

// ---- DriftAwareOffsetEstimator: query throughput (the actual hot path --
// main.cpp calls estimate_with_uncertainty_at once per event) ----

static void BM_DriftEstimator_EstimateWithUncertainty(benchmark::State& state) {
    const auto n      = static_cast<std::size_t>(state.range(0));
    const auto window = static_cast<std::size_t>(state.range(1));
    const auto& corpus = make_corpus(n, 0.0);

    DriftAwareOffsetEstimator est{DriftAwareOffsetEstimator::Config{.window_size = window, .outlier_rtt_multiplier = 3.0}};
    for (const auto& s : corpus.samples) est.add_sample(s);

    // Queries rotate over a precomputed ring so results aren't memoized away
    // by the compiler and every window position gets exercised, not just one.
    std::vector<NsTimestamp> queries;
    queries.reserve(corpus.samples.size());
    for (const auto& s : corpus.samples) queries.push_back(s.collector_send_time);

    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(est.estimate_with_uncertainty_at(queries[i]));
        i = (i + 1) % queries.size();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DriftEstimator_EstimateWithUncertainty)->ArgsProduct({{4096}, {5, 10, 25, 50}})->Unit(benchmark::kNanosecond);

static void BM_DriftEstimator_EstimateAt(benchmark::State& state) {
    const auto n      = static_cast<std::size_t>(state.range(0));
    const auto window = static_cast<std::size_t>(state.range(1));
    const auto& corpus = make_corpus(n, 0.0);

    DriftAwareOffsetEstimator est{DriftAwareOffsetEstimator::Config{.window_size = window, .outlier_rtt_multiplier = 3.0}};
    for (const auto& s : corpus.samples) est.add_sample(s);

    std::vector<NsTimestamp> queries;
    queries.reserve(corpus.samples.size());
    for (const auto& s : corpus.samples) queries.push_back(s.collector_send_time);

    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(est.estimate_at(queries[i]));
        i = (i + 1) % queries.size();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DriftEstimator_EstimateAt)->ArgsProduct({{4096}, {5, 10, 25, 50}})->Unit(benchmark::kNanosecond);

// ---- Naive OffsetEstimator, for direct throughput comparison ----
// (Accuracy is already compared in the Phase 3 README; this isolates cost.)

static void BM_NaiveOffsetEstimator_AddSample(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto& corpus = make_corpus(n, 0.0);

    for (auto _ : state) {
        OffsetEstimator est;
        for (const auto& s : corpus.samples) est.add_sample(s);
        benchmark::DoNotOptimize(est.sample_count());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_NaiveOffsetEstimator_AddSample)->Arg(64)->Arg(512)->Arg(4096)->Unit(benchmark::kMicrosecond);

static void BM_NaiveOffsetEstimator_EstimateAt(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto& corpus = make_corpus(n, 0.0);

    OffsetEstimator est;
    for (const auto& s : corpus.samples) est.add_sample(s);

    std::vector<NsTimestamp> queries;
    queries.reserve(corpus.samples.size());
    for (const auto& s : corpus.samples) queries.push_back(s.collector_send_time);

    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(est.estimate_at(queries[i]));
        i = (i + 1) % queries.size();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NaiveOffsetEstimator_EstimateAt)->Arg(64)->Arg(512)->Arg(4096)->Unit(benchmark::kNanosecond);
