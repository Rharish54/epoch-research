#include "simulation/event_source.hpp"

#include <gtest/gtest.h>
#include <unordered_set>

using namespace chronos;

namespace {

constexpr NsTimestamp kRefTime = 1'000'000'000LL;

EventSource make_source(std::uint64_t clock_seed, std::uint64_t src_seed,
                         NsDuration mean_inter_event_ns = 1'000) {
    ClockConfig clock_cfg{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = clock_seed};
    EventSourceConfig src_cfg{
        .start_time_ns = kRefTime, .mean_inter_event_ns = mean_inter_event_ns, .seed = src_seed};
    return EventSource(static_cast<SourceId>(1), clock_cfg, src_cfg, kRefTime);
}

} // namespace

TEST(EventSource, ZeroEventsReturnsEmpty) {
    auto src = make_source(1, 1);
    EXPECT_TRUE(src.generate(0).empty());
}

TEST(EventSource, TrueTimeStrictlyIncreasing) {
    auto src = make_source(1, 1);
    auto events = src.generate(200);
    ASSERT_EQ(events.size(), 200u);
    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_GT(events[i].true_time, events[i - 1].true_time)
            << "not strictly increasing at index " << i;
    }
}

TEST(EventSource, EventIdsIncrementSequentially) {
    auto src = make_source(1, 1);
    auto events = src.generate(50);
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint64_t>(events[i].event_id), i);
    }
}

TEST(EventSource, SourceIdIsConstantAcrossEvents) {
    auto src = make_source(1, 1);
    auto events = src.generate(30);
    for (const auto& e : events) {
        EXPECT_EQ(e.source_id, static_cast<SourceId>(1));
    }
}

TEST(EventSource, DeterministicWithFixedSeeds) {
    auto src1 = make_source(42, 99);
    auto src2 = make_source(42, 99);
    auto e1 = src1.generate(100);
    auto e2 = src2.generate(100);
    ASSERT_EQ(e1.size(), e2.size());
    for (std::size_t i = 0; i < e1.size(); ++i) {
        EXPECT_EQ(e1[i].true_time, e2[i].true_time);
        EXPECT_EQ(e1[i].source_local_time, e2[i].source_local_time);
        EXPECT_EQ(e1[i].event_type, e2[i].event_type);
    }
}

TEST(EventSource, DifferentSourceSeedProducesDifferentGaps) {
    auto src1 = make_source(1, 1);
    auto src2 = make_source(1, 2);
    auto e1 = src1.generate(50);
    auto e2 = src2.generate(50);
    bool any_different = false;
    for (std::size_t i = 0; i < e1.size(); ++i) {
        if (e1[i].true_time != e2[i].true_time) { any_different = true; break; }
    }
    EXPECT_TRUE(any_different);
}

TEST(EventSource, ZeroMeanInterEventTimeStillAdvances) {
    // Rate = 1/mean is infinite; the zero-gap guard must still keep true_time
    // strictly increasing rather than stalling or producing NaN/negative gaps.
    auto src = make_source(1, 1, /*mean_inter_event_ns=*/0);
    auto events = src.generate(100);
    ASSERT_EQ(events.size(), 100u);
    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_GT(events[i].true_time, events[i - 1].true_time);
    }
}

TEST(EventSource, EventTypeWithinValidRange) {
    auto src = make_source(1, 1);
    auto events = src.generate(200);
    for (const auto& e : events) {
        EXPECT_GE(static_cast<int>(e.event_type), static_cast<int>(EventType::OrderSubmitted));
        EXPECT_LE(static_cast<int>(e.event_type), static_cast<int>(EventType::Heartbeat));
    }
}

TEST(EventSource, LocalTimeMatchesClockModelOutputForZeroDistortion) {
    // With no offset/drift/noise, local time should equal true time exactly.
    auto src = make_source(1, 1);
    auto events = src.generate(20);
    for (const auto& e : events) {
        EXPECT_EQ(e.source_local_time, e.true_time);
    }
}

TEST(EventSource, MultipleGenerateCallsContinueFromPriorState) {
    // generate() is called once per source in main.cpp today, but the API
    // allows repeated calls; event ids and true_time must keep advancing
    // rather than resetting.
    auto src = make_source(1, 1);
    auto first = src.generate(10);
    auto second = src.generate(10);

    EXPECT_EQ(static_cast<std::uint64_t>(second.front().event_id), 10u);
    EXPECT_GT(second.front().true_time, first.back().true_time);
}

TEST(EventSource, DefaultStartingEventIdCollidesAcrossSources) {
    // Documents default behavior: two independent EventSources both starting
    // from starting_event_id=0 (the default) produce colliding event_id
    // sequences (0,1,2,...). Global uniqueness across sources is the
    // caller's responsibility -- see StartingEventIdProducesGloballyUniqueIds.
    auto src1 = make_source(1, 1);
    auto src2 = make_source(1, 1);
    auto e1 = src1.generate(5);
    auto e2 = src2.generate(5);
    for (std::size_t i = 0; i < e1.size(); ++i) {
        EXPECT_EQ(e1[i].event_id, e2[i].event_id);
    }
}

TEST(EventSource, StartingEventIdProducesGloballyUniqueIds) {
    ClockConfig clock_cfg1{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 1};
    EventSourceConfig src_cfg1{
        .start_time_ns = kRefTime, .mean_inter_event_ns = 1'000, .seed = 1, .starting_event_id = 0};
    EventSource src1(static_cast<SourceId>(1), clock_cfg1, src_cfg1, kRefTime);

    ClockConfig clock_cfg2{.offset_ns = 0, .drift_ppm = 0.0, .noise_std_ns = 0.0, .seed = 2};
    EventSourceConfig src_cfg2{
        .start_time_ns = kRefTime, .mean_inter_event_ns = 1'000, .seed = 2, .starting_event_id = 5};
    EventSource src2(static_cast<SourceId>(2), clock_cfg2, src_cfg2, kRefTime);

    auto e1 = src1.generate(5);
    auto e2 = src2.generate(5);

    EXPECT_EQ(static_cast<std::uint64_t>(e1.front().event_id), 0u);
    EXPECT_EQ(static_cast<std::uint64_t>(e2.front().event_id), 5u);

    std::unordered_set<std::uint64_t> ids;
    for (const auto& e : e1) ids.insert(static_cast<std::uint64_t>(e.event_id));
    for (const auto& e : e2) ids.insert(static_cast<std::uint64_t>(e.event_id));
    EXPECT_EQ(ids.size(), 10u);
}
