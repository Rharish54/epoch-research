#include "clock/offset_estimator.hpp"

#include <gtest/gtest.h>

using namespace chronos;

namespace {

SyncSample make_sample(NsTimestamp collector_send, NsTimestamp collector_receive, NsDuration offset) {
    SyncSample s;
    s.source_id              = static_cast<SourceId>(1);
    s.collector_send_time    = collector_send;
    s.source_receive_time    = collector_send;   // unused by OffsetEstimator
    s.source_send_time       = collector_receive; // unused by OffsetEstimator
    s.collector_receive_time = collector_receive;
    s.estimated_offset       = offset;
    s.round_trip_delay       = collector_receive - collector_send;
    return s;
}

} // namespace

TEST(OffsetEstimator, NoSamplesReturnsNullopt) {
    OffsetEstimator estimator;
    EXPECT_EQ(estimator.sample_count(), 0u);
    EXPECT_FALSE(estimator.estimate_at(1'000).has_value());
}

TEST(OffsetEstimator, SingleSampleUsedForAnyQueryAtOrAfterIt) {
    OffsetEstimator estimator;
    estimator.add_sample(make_sample(1'000, 1'200, 500)); // midpoint = 1100

    EXPECT_FALSE(estimator.estimate_at(1'099).has_value());
    ASSERT_TRUE(estimator.estimate_at(1'100).has_value());
    EXPECT_EQ(*estimator.estimate_at(1'100), 500);
    ASSERT_TRUE(estimator.estimate_at(1'000'000).has_value());
    EXPECT_EQ(*estimator.estimate_at(1'000'000), 500);
}

TEST(OffsetEstimator, PicksMostRecentSampleAtOrBeforeQuery) {
    OffsetEstimator estimator;
    estimator.add_sample(make_sample(0, 200, 10));       // midpoint 100
    estimator.add_sample(make_sample(1'000, 1'200, 20)); // midpoint 1100
    estimator.add_sample(make_sample(2'000, 2'200, 30)); // midpoint 2100

    EXPECT_EQ(*estimator.estimate_at(500), 10);
    EXPECT_EQ(*estimator.estimate_at(1'500), 20);
    EXPECT_EQ(*estimator.estimate_at(5'000), 30);
}

TEST(OffsetEstimator, ExactBoundaryUsesThatSample) {
    OffsetEstimator estimator;
    estimator.add_sample(make_sample(0, 200, 10));       // midpoint 100
    estimator.add_sample(make_sample(1'000, 1'200, 20)); // midpoint 1100

    EXPECT_EQ(*estimator.estimate_at(1'100), 20); // exactly at second sample's key
}

TEST(OffsetEstimator, UnaffectedByInsertionOrder) {
    OffsetEstimator in_order;
    in_order.add_sample(make_sample(0, 200, 10));
    in_order.add_sample(make_sample(1'000, 1'200, 20));
    in_order.add_sample(make_sample(2'000, 2'200, 30));

    OffsetEstimator reversed;
    reversed.add_sample(make_sample(2'000, 2'200, 30));
    reversed.add_sample(make_sample(1'000, 1'200, 20));
    reversed.add_sample(make_sample(0, 200, 10));

    for (NsTimestamp q : {50, 500, 1'500, 5'000}) {
        EXPECT_EQ(in_order.estimate_at(q), reversed.estimate_at(q))
            << "mismatch at query_time=" << q;
    }
    EXPECT_EQ(in_order.sample_count(), reversed.sample_count());
}
