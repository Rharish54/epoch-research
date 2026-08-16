#include "network/latency_model.hpp"

#include <gtest/gtest.h>

using namespace chronos;

TEST(LatencyModel, ZeroJitterAndAsymmetryYieldsExactBase) {
    LatencyConfig cfg{.base_ns = 100'000, .asymmetry_ns = 0, .jitter_std_ns = 0.0,
                       .spike_probability = 0.0, .seed = 1};
    LatencyModel model(cfg);

    EXPECT_EQ(model.sample_latency(LinkDirection::Outbound), 100'000);
    EXPECT_EQ(model.sample_latency(LinkDirection::Inbound), 100'000);
}

TEST(LatencyModel, AsymmetrySplitsEvenlyBetweenDirections) {
    // outbound - inbound should equal asymmetry_ns exactly (no jitter).
    LatencyConfig cfg{.base_ns = 100'000, .asymmetry_ns = 20'000, .jitter_std_ns = 0.0,
                       .spike_probability = 0.0, .seed = 1};
    LatencyModel model(cfg);

    const NsDuration outbound = model.sample_latency(LinkDirection::Outbound);
    const NsDuration inbound  = model.sample_latency(LinkDirection::Inbound);
    EXPECT_EQ(outbound - inbound, 20'000);
    EXPECT_EQ(outbound, 110'000);
    EXPECT_EQ(inbound, 90'000);
}

TEST(LatencyModel, LatencyNeverNegativeUnderLargeJitter) {
    // jitter std-dev far exceeds base latency, so plenty of draws would go
    // negative without the clamp.
    LatencyConfig cfg{.base_ns = 100, .asymmetry_ns = 0, .jitter_std_ns = 100'000.0,
                       .spike_probability = 0.0, .seed = 42};
    LatencyModel model(cfg);

    for (int i = 0; i < 1000; ++i) {
        EXPECT_GE(model.sample_latency(LinkDirection::Outbound), 1);
    }
}

TEST(LatencyModel, DeterministicWithFixedSeed) {
    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 50'000.0, .seed = 7};
    LatencyModel m1(cfg);
    LatencyModel m2(cfg);

    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(m1.sample_latency(LinkDirection::Outbound), m2.sample_latency(LinkDirection::Outbound));
    }
}

TEST(LatencyModel, DifferentSeedsProduceDifferentJitter) {
    LatencyConfig cfg1{.base_ns = 100'000, .jitter_std_ns = 50'000.0, .seed = 1};
    LatencyConfig cfg2{.base_ns = 100'000, .jitter_std_ns = 50'000.0, .seed = 2};
    LatencyModel m1(cfg1);
    LatencyModel m2(cfg2);

    bool any_different = false;
    for (int i = 0; i < 20; ++i) {
        if (m1.sample_latency(LinkDirection::Outbound) != m2.sample_latency(LinkDirection::Outbound)) {
            any_different = true;
            break;
        }
    }
    EXPECT_TRUE(any_different);
}

TEST(LatencyModel, SpikesInflateLatencyByMultiplier) {
    // spike_probability=1.0 forces every draw to be a spike.
    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 0.0,
                       .spike_probability = 1.0, .spike_multiplier = 10.0, .seed = 1};
    LatencyModel model(cfg);

    EXPECT_EQ(model.sample_latency(LinkDirection::Outbound), 1'000'000);
}

TEST(LatencyModel, ZeroSpikeProbabilityNeverSpikes) {
    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 1'000.0,
                       .spike_probability = 0.0, .spike_multiplier = 100.0, .seed = 1};
    LatencyModel model(cfg);

    for (int i = 0; i < 200; ++i) {
        // With multiplier 100x, any spike would be wildly larger than base +/- jitter.
        EXPECT_LT(model.sample_latency(LinkDirection::Outbound), 200'000);
    }
}

TEST(LatencyModel, ReorderingDisabledByDefaultLeavesLatenciesUnchanged) {
    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 0.0, .reorder_probability = 0.0, .seed = 1};
    LatencyModel model(cfg);

    std::vector<NsDuration> latencies = {10, 20, 30, 40, 50, 60};
    const std::vector<NsDuration> original = latencies;
    model.apply_reordering(latencies);
    EXPECT_EQ(latencies, original);
}

TEST(LatencyModel, ReorderingWithProbabilityOneSwapsEveryAdjacentPair) {
    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 0.0, .reorder_probability = 1.0, .seed = 1};
    LatencyModel model(cfg);

    std::vector<NsDuration> latencies = {10, 20, 30, 40, 50, 60};
    model.apply_reordering(latencies);
    const std::vector<NsDuration> expected = {20, 10, 40, 30, 60, 50};
    EXPECT_EQ(latencies, expected);
}

TEST(LatencyModel, ReorderingHandlesOddSizedInputWithoutOutOfBoundsAccess) {
    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 0.0, .reorder_probability = 1.0, .seed = 1};
    LatencyModel model(cfg);

    std::vector<NsDuration> latencies = {10, 20, 30};
    model.apply_reordering(latencies);
    const std::vector<NsDuration> expected = {20, 10, 30}; // last element unpaired, untouched
    EXPECT_EQ(latencies, expected);
}
