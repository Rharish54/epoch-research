#include "network/async_collector.hpp"

#include "clock/clock_model.hpp"

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace chronos;

namespace {

constexpr NsTimestamp kRefTime = 1'000'000'000LL;

// `n` events on `source`, true_time strictly increasing by `gap_ns`,
// starting at kRefTime. source_local_time is irrelevant to the async layer
// and left equal to true_time.
std::vector<MarketEvent> make_events(SourceId source, std::size_t n, NsDuration gap_ns) {
    std::vector<MarketEvent> events;
    events.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        MarketEvent e{};
        e.event_id          = static_cast<EventId>(i);
        e.source_id          = source;
        e.event_type         = EventType::Heartbeat;
        e.true_time          = kRefTime + static_cast<NsTimestamp>(i) * gap_ns;
        e.source_local_time  = e.true_time;
        events.push_back(e);
    }
    return events;
}

} // namespace

TEST(AsyncCollector, ZeroJitterAndSpikeGivesExactReceiveTime) {
    auto events = make_events(SourceId{0}, 10, /*gap_ns=*/1'000);
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{events});

    LatencyConfig cfg{.base_ns = 50'000, .jitter_std_ns = 0.0, .spike_probability = 0.0, .seed = 1};
    LatencyModel latency(cfg);
    schedule_source_delivery(sched, collector, latency, events, /*indices_begin=*/0, /*apply_reordering=*/false);
    sched.run();

    for (const auto& e : events) {
        EXPECT_EQ(e.receive_time, e.true_time + 50'000);
    }
}

TEST(AsyncCollector, ConstantLatencyPreservesArrivalOrder) {
    auto events = make_events(SourceId{0}, 8, /*gap_ns=*/1'000);
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{events});

    LatencyConfig cfg{.base_ns = 10'000, .jitter_std_ns = 0.0, .spike_probability = 0.0, .seed = 2};
    LatencyModel latency(cfg);
    schedule_source_delivery(sched, collector, latency, events, 0, false);
    sched.run();

    const auto& order = collector.arrival_order();
    ASSERT_EQ(order.size(), 8u);
    for (std::size_t i = 0; i < order.size(); ++i) EXPECT_EQ(order[i], i);
    EXPECT_EQ(collector.stats().same_source_inversions, 0u);
}

TEST(AsyncCollector, LargeJitterWithTightGapsProducesSameSourceInversions) {
    // Jitter std-dev far exceeds the departure gap -- overtakes are all but
    // guaranteed.
    auto events = make_events(SourceId{0}, 100, /*gap_ns=*/10);
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{events});

    LatencyConfig cfg{.base_ns = 100'000, .jitter_std_ns = 200'000.0, .spike_probability = 0.0, .seed = 3};
    LatencyModel latency(cfg);
    schedule_source_delivery(sched, collector, latency, events, 0, false);
    sched.run();

    EXPECT_GT(collector.stats().same_source_inversions, 0u);
}

TEST(AsyncCollector, ApplyReorderingSwapsExpectedDelayPairingIntoRealReceiveTimes) {
    // apply_reordering swaps adjacent-pair VALUES in the delay vector -- if
    // every delay were identical (e.g. jitter off), swapping identical
    // values would be a no-op on arrival order regardless of wiring, which
    // wouldn't prove the integration actually happened. So this test uses
    // jitter (delays genuinely differ per event) and, with
    // reorder_probability=1.0 (every pair unconditionally swapped, per
    // LatencyModel::apply_reordering), predicts the exact post-swap
    // per-event delay by hand from an identically-seeded reference model,
    // then asserts the real pipeline's receive_time matches that
    // hand-computed swapped pairing exactly.
    auto events = make_events(SourceId{0}, 4, /*gap_ns=*/1'000);
    LatencyConfig cfg{.base_ns = 50'000, .jitter_std_ns = 30'000.0, .reorder_probability = 1.0, .seed = 4};

    LatencyModel reference(cfg);
    std::vector<NsDuration> hand_drawn;
    for (std::size_t i = 0; i < events.size(); ++i) hand_drawn.push_back(reference.sample_latency(LinkDirection::Inbound));
    ASSERT_EQ(hand_drawn.size(), 4u);
    std::vector<NsDuration> expected_after_swap = hand_drawn;
    std::swap(expected_after_swap[0], expected_after_swap[1]);
    std::swap(expected_after_swap[2], expected_after_swap[3]);
    // Sanity check the test fixture itself: if jitter happened to produce
    // identical adjacent delays, the swap wouldn't be observable and this
    // test would pass vacuously.
    ASSERT_NE(hand_drawn[0], hand_drawn[1]);
    ASSERT_NE(hand_drawn[2], hand_drawn[3]);

    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{events});
    LatencyModel pipeline(cfg); // identically configured, fresh instance
    schedule_source_delivery(sched, collector, pipeline, events, 0, /*apply_reordering=*/true);
    sched.run();

    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].receive_time, events[i].true_time + expected_after_swap[i]) << "at index " << i;
    }
}

TEST(AsyncCollector, CrossSourceDeliveryInterleavesOnOneSharedCollector) {
    auto events_a = make_events(SourceId{0}, 5, /*gap_ns=*/1'000);
    auto events_b = make_events(SourceId{1}, 5, /*gap_ns=*/1'000);
    std::vector<MarketEvent> all;
    all.reserve(10);
    for (auto& e : events_a) all.push_back(e);
    for (auto& e : events_b) all.push_back(e);

    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{all});

    // Large jitter so the two sources' deliveries genuinely interleave
    // rather than trivially arriving in two separate blocks.
    LatencyConfig cfg_a{.base_ns = 50'000, .jitter_std_ns = 100'000.0, .seed = 5};
    LatencyConfig cfg_b{.base_ns = 50'000, .jitter_std_ns = 100'000.0, .seed = 6};
    LatencyModel latency_a(cfg_a), latency_b(cfg_b);
    schedule_source_delivery(sched, collector, latency_a, std::span<const MarketEvent>{all}.subspan(0, 5), 0, false);
    schedule_source_delivery(sched, collector, latency_b, std::span<const MarketEvent>{all}.subspan(5, 5), 5, false);
    sched.run();

    const auto& order = collector.arrival_order();
    ASSERT_EQ(order.size(), 10u);
    bool interleaved = false;
    for (std::size_t i = 1; i + 1 < order.size(); ++i) {
        const bool a_neighbor_before = order[i - 1] < 5;
        const bool b_here            = order[i] >= 5;
        const bool a_neighbor_after  = order[i + 1] < 5;
        if (a_neighbor_before && b_here && a_neighbor_after) interleaved = true;
    }
    EXPECT_TRUE(interleaved);
}

TEST(AsyncCollector, ZeroReorderProbabilityReproducesHandDrawnSequentialLatencies) {
    // Migration guarantee: at reorder_probability=0, every receive_time
    // equals true_time plus a latency drawn by hand, in index order, from an
    // identically-configured standalone LatencyModel. This is the formal
    // statement that Phase 6 does not change any receive_time VALUE at the
    // default setting -- only delivery order/interleaving.
    auto events = make_events(SourceId{0}, 20, /*gap_ns=*/500);
    LatencyConfig cfg{.base_ns = 80'000, .jitter_std_ns = 30'000.0, .spike_probability = 0.1, .seed = 7};

    LatencyModel reference(cfg);
    std::vector<NsDuration> hand_drawn;
    hand_drawn.reserve(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) hand_drawn.push_back(reference.sample_latency(LinkDirection::Inbound));

    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{events});
    LatencyModel pipeline(cfg); // identically configured, fresh instance
    schedule_source_delivery(sched, collector, pipeline, events, 0, /*apply_reordering=*/false);
    sched.run();

    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].receive_time, events[i].true_time + hand_drawn[i]) << "at index " << i;
    }
}

TEST(AsyncCollector, IdenticalSeedsProduceIdenticalDeliveryAndArrivalOrder) {
    auto run = [] {
        auto events = make_events(SourceId{0}, 30, /*gap_ns=*/200);
        boost::asio::io_context io;
        VirtualTimeScheduler sched(io);
        AsyncCollector collector(std::span<MarketEvent>{events});
        LatencyConfig cfg{.jitter_std_ns = 40'000.0, .spike_probability = 0.1, .reorder_probability = 0.3, .seed = 8};
        LatencyModel latency(cfg);
        schedule_source_delivery(sched, collector, latency, events, 0, true);
        sched.run();
        return std::pair{events, collector.arrival_order()};
    };

    const auto [events_a, order_a] = run();
    const auto [events_b, order_b] = run();

    ASSERT_EQ(events_a.size(), events_b.size());
    for (std::size_t i = 0; i < events_a.size(); ++i) {
        EXPECT_EQ(events_a[i].receive_time, events_b[i].receive_time) << "at index " << i;
    }
    EXPECT_EQ(order_a, order_b);
}

TEST(AsyncCollector, DeliverySchedulingDoesNotPerturbAnUnrelatedClockModelsEventNoiseChannel) {
    // Analogue of sync_exchange_test.cpp's EventNoiseChannelIsUnaffectedBySyncExchanges:
    // an unrelated ClockModel's Event-channel sequence must be identical
    // whether or not a full delivery schedule ran elsewhere, since the two
    // subsystems own entirely separate RNG state.
    ClockConfig clock_cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 25.0, .seed = 99};

    ClockModel untouched(SourceId{0}, clock_cfg, kRefTime);
    const NsTimestamp before = untouched.to_local(kRefTime + 500, NoiseChannel::Event);

    auto events = make_events(SourceId{0}, 50, /*gap_ns=*/100);
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    AsyncCollector collector(std::span<MarketEvent>{events});
    LatencyConfig cfg{.jitter_std_ns = 50'000.0, .spike_probability = 0.2, .reorder_probability = 0.5, .seed = 10};
    LatencyModel latency(cfg);
    schedule_source_delivery(sched, collector, latency, events, 0, true);
    sched.run();

    ClockModel exercised(SourceId{0}, clock_cfg, kRefTime);
    const NsTimestamp after = exercised.to_local(kRefTime + 500, NoiseChannel::Event);

    EXPECT_EQ(before, after);
}

TEST(AsyncCollector, ReorderingPermutesLatencyPairingsOnlyNotTheMultiset) {
    auto run_latencies = [](bool reorder) {
        auto events = make_events(SourceId{0}, 40, /*gap_ns=*/300);
        boost::asio::io_context io;
        VirtualTimeScheduler sched(io);
        AsyncCollector collector(std::span<MarketEvent>{events});
        LatencyConfig cfg{
            .jitter_std_ns = 20'000.0, .spike_probability = 0.1, .reorder_probability = reorder ? 1.0 : 0.0, .seed = 11};
        LatencyModel latency(cfg);
        schedule_source_delivery(sched, collector, latency, events, 0, reorder);
        sched.run();

        std::vector<double> realized(collector.realized_latency_ns());
        std::sort(realized.begin(), realized.end());
        return realized;
    };

    EXPECT_EQ(run_latencies(false), run_latencies(true));
}
