#include "simulation/sync_exchange.hpp"

#include <gtest/gtest.h>

using namespace chronos;

namespace {
constexpr NsTimestamp kRefTime = 1'000'000'000LL;
} // namespace

TEST(SyncExchange, ExactOffsetUnderSymmetricLatencyZeroNoiseZeroDrift) {
    // Headline correctness test: with symmetric one-way latency and no clock
    // noise/drift, the estimator recovers the true offset exactly. This is
    // the analytic guarantee the whole sync design rests on.
    constexpr NsDuration kTrueOffset = 5'000;
    ClockConfig clock_cfg{.offset_ns = kTrueOffset, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 1};
    ClockModel source_clock(static_cast<SourceId>(1), clock_cfg, kRefTime);

    LatencyConfig latency_cfg{.base_ns = 100'000, .asymmetry_ns = 0, .jitter_std_ns = 0.0,
                               .spike_probability = 0.0, .seed = 1};
    LatencyModel latency(latency_cfg);

    const SyncSample sample = run_sync_exchange(static_cast<SourceId>(1), kRefTime + 10'000'000,
                                                 source_clock, latency);

    EXPECT_EQ(sample.estimated_offset, kTrueOffset);
}

TEST(SyncExchange, RoundTripDelayEqualsSumOfOneWayDelaysUnderZeroDistortion) {
    ClockConfig clock_cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 1};
    ClockModel source_clock(static_cast<SourceId>(1), clock_cfg, kRefTime);

    LatencyConfig latency_cfg{.base_ns = 100'000, .asymmetry_ns = 0, .jitter_std_ns = 0.0,
                               .spike_probability = 0.0, .seed = 1};
    LatencyModel latency(latency_cfg);

    const SyncSample sample = run_sync_exchange(static_cast<SourceId>(1), kRefTime, source_clock, latency);

    // Zero jitter, zero asymmetry -> both directions are exactly base_ns.
    EXPECT_EQ(sample.round_trip_delay, 200'000);
}

TEST(SyncExchange, AsymmetryProducesExactlyHalfAsymmetryError) {
    constexpr NsDuration kTrueOffset = 5'000;
    constexpr NsDuration kAsymmetry  = 30'000; // outbound - inbound
    ClockConfig clock_cfg{.offset_ns = kTrueOffset, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 1};
    ClockModel source_clock(static_cast<SourceId>(1), clock_cfg, kRefTime);

    LatencyConfig latency_cfg{.base_ns = 100'000, .asymmetry_ns = kAsymmetry, .jitter_std_ns = 0.0,
                               .spike_probability = 0.0, .seed = 1};
    LatencyModel latency(latency_cfg);

    const SyncSample sample = run_sync_exchange(static_cast<SourceId>(1), kRefTime, source_clock, latency);

    // error = (d_out - d_in) / 2 = asymmetry / 2.
    EXPECT_EQ(sample.estimated_offset - kTrueOffset, kAsymmetry / 2);
}

TEST(SyncExchange, DeterministicForFixedSeeds) {
    ClockConfig clock_cfg{.offset_ns = 1'000, .drift_ppm = 50.0, .noise_std_ns = 20.0, .seed = 3};
    LatencyConfig latency_cfg{.base_ns = 100'000, .jitter_std_ns = 30'000.0, .seed = 4};

    ClockModel clock1(static_cast<SourceId>(1), clock_cfg, kRefTime);
    LatencyModel lat1(latency_cfg);
    const SyncSample s1 = run_sync_exchange(static_cast<SourceId>(1), kRefTime + 500'000, clock1, lat1);

    ClockModel clock2(static_cast<SourceId>(1), clock_cfg, kRefTime);
    LatencyModel lat2(latency_cfg);
    const SyncSample s2 = run_sync_exchange(static_cast<SourceId>(1), kRefTime + 500'000, clock2, lat2);

    EXPECT_EQ(s1.estimated_offset, s2.estimated_offset);
    EXPECT_EQ(s1.round_trip_delay, s2.round_trip_delay);
    EXPECT_EQ(s1.source_receive_time, s2.source_receive_time);
    EXPECT_EQ(s1.source_send_time, s2.source_send_time);
}

TEST(SyncExchange, ScheduleProducesOneSamplePerInterval) {
    ClockConfig clock_cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 1};
    ClockModel source_clock(static_cast<SourceId>(1), clock_cfg, kRefTime);
    LatencyConfig latency_cfg{.base_ns = 100'000, .jitter_std_ns = 0.0, .seed = 1};
    LatencyModel latency(latency_cfg);

    const auto samples = run_sync_schedule(static_cast<SourceId>(1), kRefTime, kRefTime + 1'000'000,
                                            /*interval_ns=*/200'000, source_clock, latency);

    EXPECT_EQ(samples.size(), 5u); // [0, 200k, 400k, 600k, 800k) -> 5 ticks before 1,000,000
    for (std::size_t i = 0; i < samples.size(); ++i) {
        EXPECT_EQ(samples[i].collector_send_time, kRefTime + static_cast<NsTimestamp>(i) * 200'000);
    }
}

TEST(SyncExchange, ZeroOrNegativeIntervalProducesNoSamples) {
    ClockConfig clock_cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 1};
    ClockModel source_clock(static_cast<SourceId>(1), clock_cfg, kRefTime);
    LatencyConfig latency_cfg{.base_ns = 100'000, .seed = 1};
    LatencyModel latency(latency_cfg);

    EXPECT_TRUE(run_sync_schedule(static_cast<SourceId>(1), kRefTime, kRefTime + 1'000'000,
                                   /*interval_ns=*/0, source_clock, latency).empty());
    EXPECT_TRUE(run_sync_schedule(static_cast<SourceId>(1), kRefTime, kRefTime + 1'000'000,
                                   /*interval_ns=*/-100, source_clock, latency).empty());
}

TEST(SyncExchange, EventNoiseChannelIsUnaffectedBySyncExchanges) {
    // Regression guard for the dual-noise-stream fix: running sync exchanges
    // against a ClockModel must not perturb what to_local(..., Event) would
    // have returned, since EventSource relies on that determinism.
    ClockConfig clock_cfg{.offset_ns = 100, .drift_ppm = 10.0, .noise_std_ns = 50.0, .seed = 9};

    ClockModel untouched(static_cast<SourceId>(1), clock_cfg, kRefTime);
    const NsTimestamp expected = untouched.to_local(kRefTime + 1'000'000, NoiseChannel::Event);

    ClockModel exercised(static_cast<SourceId>(1), clock_cfg, kRefTime);
    LatencyConfig latency_cfg{.base_ns = 100'000, .jitter_std_ns = 10'000.0, .seed = 2};
    LatencyModel latency(latency_cfg);
    // Run several sync exchanges against `exercised` first.
    run_sync_schedule(static_cast<SourceId>(1), kRefTime, kRefTime + 900'000, 100'000, exercised, latency);
    const NsTimestamp actual = exercised.to_local(kRefTime + 1'000'000, NoiseChannel::Event);

    EXPECT_EQ(actual, expected);
}
