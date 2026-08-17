#include "network/virtual_scheduler.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace chronos;

TEST(VirtualTimeScheduler, FiresActionsInSimulatedTimeOrderRegardlessOfInsertionOrder) {
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    std::vector<NsTimestamp> fired;

    sched.schedule_at(30, [&](NsTimestamp t) { fired.push_back(t); });
    sched.schedule_at(10, [&](NsTimestamp t) { fired.push_back(t); });
    sched.schedule_at(20, [&](NsTimestamp t) { fired.push_back(t); });

    const auto executed = sched.run();
    EXPECT_EQ(executed, 3u);
    ASSERT_EQ(fired.size(), 3u);
    EXPECT_EQ(fired[0], 10);
    EXPECT_EQ(fired[1], 20);
    EXPECT_EQ(fired[2], 30);
}

TEST(VirtualTimeScheduler, EqualTimestampsBreakTiesByInsertionOrder) {
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    std::vector<int> fired;

    for (int i = 0; i < 5; ++i) {
        sched.schedule_at(100, [&, i](NsTimestamp) { fired.push_back(i); });
    }
    sched.run();

    ASSERT_EQ(fired.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(fired[static_cast<std::size_t>(i)], i);
}

TEST(VirtualTimeScheduler, ActionScheduledFromWithinAnActionIsHonoredInSimulatedTimeOrder) {
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    std::vector<NsTimestamp> fired;

    // A handler running at t=10 schedules two more actions: one "in the
    // past" (t=5) and one further out (t=50). Even a scheduled-into-the-past
    // action must fire in (fire_time, seq) order like anything else -- the
    // scheduler doesn't special-case it, it simply becomes the new earliest
    // pending entry and fires next.
    sched.schedule_at(10, [&](NsTimestamp t) {
        fired.push_back(t);
        sched.schedule_at(5, [&](NsTimestamp t2) { fired.push_back(t2); });
        sched.schedule_at(50, [&](NsTimestamp t2) { fired.push_back(t2); });
    });

    sched.run();

    ASSERT_EQ(fired.size(), 3u);
    EXPECT_EQ(fired[0], 10);
    EXPECT_EQ(fired[1], 5);
    EXPECT_EQ(fired[2], 50);
}

TEST(VirtualTimeScheduler, RunDrainsTheQueueAndReturnsExecutedCount) {
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    sched.schedule_at(1, [](NsTimestamp) {});
    sched.schedule_at(2, [](NsTimestamp) {});

    EXPECT_EQ(sched.pending(), 2u);
    const auto executed = sched.run();
    EXPECT_EQ(executed, 2u);
    EXPECT_EQ(sched.pending(), 0u);
}

TEST(VirtualTimeScheduler, EmptySchedulerRunIsNoOp) {
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    EXPECT_EQ(sched.run(), 0u);
    EXPECT_EQ(sched.pending(), 0u);
}

TEST(VirtualTimeScheduler, RepeatedRunAfterMoreSchedulingStillExecutes) {
    // Regression guard for the io_context "stopped" trap: io_context
    // latches stopped once run() returns for lack of work, and a second
    // run() on a stopped context is a silent no-op unless restart() is
    // called first. If VirtualTimeScheduler::run() ever forgets that
    // restart(), this second round would deliver zero actions instead of one.
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);

    sched.schedule_at(1, [](NsTimestamp) {});
    EXPECT_EQ(sched.run(), 1u);

    bool second_fired = false;
    sched.schedule_at(2, [&](NsTimestamp) { second_fired = true; });
    EXPECT_EQ(sched.run(), 1u);
    EXPECT_TRUE(second_fired);
}

TEST(VirtualTimeScheduler, NowReflectsTheCurrentlyFiringAction) {
    boost::asio::io_context io;
    VirtualTimeScheduler sched(io);
    NsTimestamp observed = -1;
    sched.schedule_at(42, [&](NsTimestamp) { observed = sched.now(); });
    sched.run();
    EXPECT_EQ(observed, 42);
}
