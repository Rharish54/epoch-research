#include "clock/clock_model.hpp"

#include <gtest/gtest.h>
#include <cstdint>

using namespace chronos;

namespace {

constexpr NsTimestamp kRefTime = 0;
constexpr NsTimestamp k1SecNs  = 1'000'000'000LL;

} // namespace

// ---- Zero everything -------------------------------------------------------

TEST(ClockModel, IdentityWithNoDistortion) {
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    EXPECT_EQ(model.to_local(k1SecNs), k1SecNs);
}

// ---- Offset ----------------------------------------------------------------

TEST(ClockModel, PositiveOffsetApplied) {
    ClockConfig cfg{.offset_ns = 1'000, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    EXPECT_EQ(model.to_local(k1SecNs), k1SecNs + 1'000);
}

TEST(ClockModel, NegativeOffsetApplied) {
    ClockConfig cfg{.offset_ns = -2'000, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    EXPECT_EQ(model.to_local(k1SecNs), k1SecNs - 2'000);
}

TEST(ClockModel, OffsetIsZeroAtReferenceTime) {
    // With zero offset and zero drift, local == true at any time.
    ClockConfig cfg{.offset_ns = 500, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    // Offset is constant, so it appears even at reference time.
    EXPECT_EQ(model.to_local(kRefTime), kRefTime + 500);
}

// ---- Drift -----------------------------------------------------------------

TEST(ClockModel, PositiveDriftAccumulates) {
    // 1000 PPM: clock gains 1000 ns per millisecond of true time elapsed.
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = 1000.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    // After 1 second: drift = 1000 * 1e9 / 1e6 = 1e6 ns = 1 ms.
    const NsDuration expected_drift = static_cast<NsDuration>(1000.0 * k1SecNs / 1'000'000.0);
    EXPECT_EQ(model.to_local(k1SecNs), k1SecNs + expected_drift);
}

TEST(ClockModel, NegativeDriftAccumulates) {
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = -500.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    const NsDuration expected_drift = static_cast<NsDuration>(-500.0 * k1SecNs / 1'000'000.0);
    EXPECT_EQ(model.to_local(k1SecNs), k1SecNs + expected_drift);
}

TEST(ClockModel, DriftIsProportionalToElapsed) {
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = 100.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    const NsTimestamp t1 = k1SecNs;
    const NsTimestamp t2 = 2 * k1SecNs;

    const NsTimestamp local1 = model.to_local(t1);
    const NsTimestamp local2 = model.to_local(t2);

    // drift at t2 should be twice the drift at t1.
    const NsDuration d1 = local1 - t1;
    const NsDuration d2 = local2 - t2;
    EXPECT_EQ(d2, 2 * d1);
}

TEST(ClockModel, ZeroDriftAtReferenceTime) {
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = 1000.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    // elapsed = 0, so drift = 0.
    EXPECT_EQ(model.to_local(kRefTime), kRefTime);
}

// ---- Noise -----------------------------------------------------------------

TEST(ClockModel, DeterministicNoiseWithFixedSeed) {
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 200.0, .seed = 42};

    ClockModel m1(static_cast<SourceId>(1), cfg, kRefTime);
    ClockModel m2(static_cast<SourceId>(1), cfg, kRefTime);

    // Same seed → same sequence.
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(m1.to_local(k1SecNs * i), m2.to_local(k1SecNs * i));
    }
}

TEST(ClockModel, DifferentSeedsProduceDifferentNoise) {
    ClockConfig cfg1{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 200.0, .seed = 1};
    ClockConfig cfg2{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 200.0, .seed = 2};

    ClockModel m1(static_cast<SourceId>(1), cfg1, kRefTime);
    ClockModel m2(static_cast<SourceId>(1), cfg2, kRefTime);

    // Seeds differ → outputs should differ (with overwhelming probability).
    bool any_different = false;
    for (int i = 0; i < 10; ++i) {
        if (m1.to_local(k1SecNs * i) != m2.to_local(k1SecNs * i)) {
            any_different = true;
            break;
        }
    }
    EXPECT_TRUE(any_different);
}

TEST(ClockModel, ZeroNoiseStdProducesNoNoise) {
    ClockConfig cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 99};
    ClockModel m(static_cast<SourceId>(1), cfg, kRefTime);

    // With noise_std_ns == 0 the clock should be noiseless.
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(m.to_local(k1SecNs * i), k1SecNs * i);
    }
}

// ---- Combined offset + drift -----------------------------------------------

TEST(ClockModel, OffsetAndDriftCombine) {
    ClockConfig cfg{.offset_ns = 1'000, .drift_ppm = 1000.0, .noise_std_ns = 0.0, .seed = 0};
    ClockModel model(static_cast<SourceId>(1), cfg, kRefTime);

    const NsDuration expected_drift = static_cast<NsDuration>(1000.0 * k1SecNs / 1'000'000.0);
    EXPECT_EQ(model.to_local(k1SecNs), k1SecNs + 1'000 + expected_drift);
}
