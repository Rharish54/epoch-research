#include "metrics/percentile.hpp"

#include <gtest/gtest.h>

using namespace chronos;

TEST(Percentile, EmptyInputReturnsZero) {
    EXPECT_DOUBLE_EQ(percentile({}, 0.5), 0.0);
}

TEST(Percentile, SingleValueReturnsThatValue) {
    EXPECT_DOUBLE_EQ(percentile({42.0}, 0.5), 42.0);
    EXPECT_DOUBLE_EQ(percentile({42.0}, 0.0), 42.0);
    EXPECT_DOUBLE_EQ(percentile({42.0}, 1.0), 42.0);
}

TEST(Percentile, MedianOfOddLengthArray) {
    // sorted: 1 2 3 4 5 -> nearest-rank median is the middle element.
    EXPECT_DOUBLE_EQ(percentile({5, 1, 4, 2, 3}, 0.5), 3.0);
}

TEST(Percentile, MedianOfEvenLengthArray) {
    // sorted: 1 2 3 4 -> nearest-rank at p=0.5 rounds to index 2 (value 3).
    EXPECT_DOUBLE_EQ(percentile({4, 2, 1, 3}, 0.5), 3.0);
}

TEST(Percentile, ZeroPercentileReturnsMinimum) {
    EXPECT_DOUBLE_EQ(percentile({5, 1, 4, 2, 3}, 0.0), 1.0);
}

TEST(Percentile, OnePercentileReturnsMaximum) {
    EXPECT_DOUBLE_EQ(percentile({5, 1, 4, 2, 3}, 1.0), 5.0);
}

TEST(Percentile, P95And99OnKnownArray) {
    // 0..99, sorted. p95 -> index round(0.95*99)=94 -> value 94.
    // p99 -> index round(0.99*99)=98 -> value 98.
    std::vector<double> values;
    for (int i = 0; i < 100; ++i) values.push_back(static_cast<double>(i));

    EXPECT_DOUBLE_EQ(percentile(values, 0.95), 94.0);
    EXPECT_DOUBLE_EQ(percentile(values, 0.99), 98.0);
}

TEST(Percentile, OutOfRangePIsClamped) {
    EXPECT_DOUBLE_EQ(percentile({5, 1, 4, 2, 3}, -1.0), 1.0);
    EXPECT_DOUBLE_EQ(percentile({5, 1, 4, 2, 3}, 2.0), 5.0);
}
