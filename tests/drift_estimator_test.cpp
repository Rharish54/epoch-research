#include "clock/drift_estimator.hpp"

#include <gtest/gtest.h>
#include <cmath>

using namespace chronos;

namespace {

constexpr NsTimestamp kRefTime = 1'000'000'000LL;

// A SyncSample carrying an exact estimated_offset/round_trip_delay, with
// collector_send/receive_time chosen so the key ((send+receive)/2) lands
// exactly on `key`.
SyncSample make_sample(NsTimestamp key, NsDuration offset, NsDuration rtt) {
    SyncSample s;
    s.source_id              = static_cast<SourceId>(1);
    s.collector_send_time    = key;
    s.collector_receive_time = key; // send == receive == key -> midpoint == key
    s.source_receive_time    = key;
    s.source_send_time       = key;
    s.estimated_offset       = offset;
    s.round_trip_delay       = rtt;
    return s;
}

} // namespace

TEST(DriftEstimator, NoSamplesReturnsNullopt) {
    DriftAwareOffsetEstimator est;
    EXPECT_FALSE(est.estimate_at(kRefTime).has_value());
    EXPECT_EQ(est.accepted_count(), 0u);
    EXPECT_EQ(est.rejected_count(), 0u);
}

TEST(DriftEstimator, RecoversKnownPositiveDriftExactlyUnderZeroNoise) {
    // offset(t) = 1000 + 2*(t - kRefTime), equal RTT on every sample so
    // weights are uniform -> ordinary least squares should fit exactly.
    DriftAwareOffsetEstimator est;
    for (int i = 0; i < 10; ++i) {
        const NsTimestamp t = kRefTime + i * 100'000; // 100us apart
        const NsDuration offset = 1000 + 2 * (t - kRefTime);
        est.add_sample(make_sample(t, offset, /*rtt=*/50'000));
    }

    const NsTimestamp query = kRefTime + 5 * 100'000;
    ASSERT_TRUE(est.estimate_at(query).has_value());
    EXPECT_NEAR(static_cast<double>(*est.estimate_at(query)), 1000.0 + 2.0 * 5 * 100'000, 1.0);
}

TEST(DriftEstimator, RecoversKnownNegativeDriftExactlyUnderZeroNoise) {
    DriftAwareOffsetEstimator est;
    for (int i = 0; i < 10; ++i) {
        const NsTimestamp t = kRefTime + i * 100'000;
        const NsDuration offset = 5000 - 3 * (t - kRefTime);
        est.add_sample(make_sample(t, offset, /*rtt=*/50'000));
    }

    const NsTimestamp query = kRefTime + 7 * 100'000;
    ASSERT_TRUE(est.estimate_at(query).has_value());
    EXPECT_NEAR(static_cast<double>(*est.estimate_at(query)), 5000.0 - 3.0 * 7 * 100'000, 1.0);
}

TEST(DriftEstimator, WeightedFitStaysCloserToTrueLineThanAnySingleNoisySample) {
    // True line: offset(t) = 1000 + 1*(t - kRefTime). Deterministic scatter
    // alternates +/-2000ns around it (no randomness -> no flakiness).
    DriftAwareOffsetEstimator est;
    const std::vector<NsDuration> scatter = {2000, -2000, 2000, -2000, 2000, -2000};
    for (std::size_t i = 0; i < scatter.size(); ++i) {
        const NsTimestamp t = kRefTime + static_cast<NsTimestamp>(i) * 100'000;
        const NsDuration true_offset = 1000 + static_cast<NsDuration>(i) * 100'000;
        est.add_sample(make_sample(t, true_offset + scatter[i], /*rtt=*/50'000));
    }

    const NsTimestamp query = kRefTime + 5 * 100'000;
    const NsDuration true_at_query = 1000 + 5 * 100'000;
    ASSERT_TRUE(est.estimate_at(query).has_value());
    const double error = std::abs(static_cast<double>(*est.estimate_at(query)) - true_at_query);
    // Any single noisy sample is off by exactly 2000ns; the fit over 6
    // alternating-scatter points should do meaningfully better.
    EXPECT_LT(error, 2000.0);
}

TEST(DriftEstimator, SlidingWindowEvictsOldSamples) {
    DriftAwareOffsetEstimator est(DriftAwareOffsetEstimator::Config{.window_size = 3, .outlier_rtt_multiplier = 3.0});

    // First sample carries a wildly wrong offset -- if it stayed in the
    // window forever, it would drag every later estimate off the true line.
    est.add_sample(make_sample(kRefTime, /*offset=*/1'000'000, /*rtt=*/50'000));
    est.add_sample(make_sample(kRefTime + 100'000, /*offset=*/1'000'000, /*rtt=*/50'000));
    est.add_sample(make_sample(kRefTime + 200'000, /*offset=*/1'000'000, /*rtt=*/50'000));
    // Now feed enough consistent, correct samples to evict the first entry
    // from a window_size=3 store.
    est.add_sample(make_sample(kRefTime + 300'000, /*offset=*/100, /*rtt=*/50'000));
    est.add_sample(make_sample(kRefTime + 400'000, /*offset=*/100, /*rtt=*/50'000));
    est.add_sample(make_sample(kRefTime + 500'000, /*offset=*/100, /*rtt=*/50'000));

    ASSERT_TRUE(est.estimate_at(kRefTime + 500'000).has_value());
    // Window now holds only the three offset=100 samples -> estimate should
    // be close to 100, not dragged toward 1,000,000 by the evicted outlier.
    EXPECT_NEAR(static_cast<double>(*est.estimate_at(kRefTime + 500'000)), 100.0, 1.0);
}

TEST(DriftEstimator, RejectsSampleWithAbnormallyHighRtt) {
    DriftAwareOffsetEstimator est;
    // Establish history: consistent low RTT, offset near 100.
    for (int i = 0; i < 5; ++i) {
        est.add_sample(make_sample(kRefTime + i * 100'000, /*offset=*/100, /*rtt=*/10'000));
    }
    ASSERT_EQ(est.accepted_count(), 5u);
    ASSERT_EQ(est.rejected_count(), 0u);

    // RTT is 100x the established median -> should be rejected, along with
    // its deliberately corrupt offset value.
    est.add_sample(make_sample(kRefTime + 500'000, /*offset=*/9'999'999, /*rtt=*/1'000'000));

    EXPECT_EQ(est.accepted_count(), 5u);
    EXPECT_EQ(est.rejected_count(), 1u);

    ASSERT_TRUE(est.estimate_at(kRefTime + 500'000).has_value());
    EXPECT_NEAR(static_cast<double>(*est.estimate_at(kRefTime + 500'000)), 100.0, 1.0);
}

TEST(DriftEstimator, BootstrapAcceptsInitialSamplesRegardlessOfRtt) {
    DriftAwareOffsetEstimator est;
    // First 3 samples: wildly varying RTT, no history yet to judge against.
    est.add_sample(make_sample(kRefTime, /*offset=*/100, /*rtt=*/1'000));
    est.add_sample(make_sample(kRefTime + 100'000, /*offset=*/100, /*rtt=*/1'000'000));
    est.add_sample(make_sample(kRefTime + 200'000, /*offset=*/100, /*rtt=*/1));

    EXPECT_EQ(est.accepted_count(), 3u);
    EXPECT_EQ(est.rejected_count(), 0u);
}

TEST(DriftEstimator, DoesNotExtrapolateSlopeBeyondObservedWindow) {
    // A noisy slope multiplied by a large extrapolation distance can blow up
    // arbitrarily -- estimate_at should clamp to the fitted window's edge
    // instead of projecting forward indefinitely. Feed a real (small) slope,
    // then query far past the last sample: an unclamped fit would report a
    // huge offset; the clamped one should stay pinned near the last sample's
    // fitted value.
    DriftAwareOffsetEstimator est;
    for (int i = 0; i < 5; ++i) {
        const NsTimestamp t = kRefTime + i * 100'000;
        const NsDuration offset = 1000 + 5 * (t - kRefTime); // slope = 5 ns/ns
        est.add_sample(make_sample(t, offset, /*rtt=*/50'000));
    }

    const NsTimestamp last_sample_time = kRefTime + 4 * 100'000;
    const double value_at_last_sample = static_cast<double>(*est.estimate_at(last_sample_time));

    // Query 100x further out than the window itself spans.
    const NsTimestamp far_query = kRefTime + 4 * 100'000 + 100 * 400'000;
    ASSERT_TRUE(est.estimate_at(far_query).has_value());
    const double far_value = static_cast<double>(*est.estimate_at(far_query));

    EXPECT_NEAR(far_value, value_at_last_sample, 1.0);
}

TEST(DriftEstimator, LowerRttSampleWeightedMoreHeavily) {
    // Two samples at the same x (same key), very different offsets and RTTs.
    // The fit's weighted mean should sit closer to the low-RTT sample.
    DriftAwareOffsetEstimator est;
    est.add_sample(make_sample(kRefTime, /*offset=*/0, /*rtt=*/1'000));       // low RTT, trusted
    est.add_sample(make_sample(kRefTime, /*offset=*/10'000, /*rtt=*/100'000)); // high RTT, distrusted

    ASSERT_TRUE(est.estimate_at(kRefTime).has_value());
    const double estimate = static_cast<double>(*est.estimate_at(kRefTime));
    // Unweighted mean would be 5000; weighting toward the low-RTT sample
    // should pull the result well below the midpoint, toward 0.
    EXPECT_LT(estimate, 2500.0);
}
