#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "core/timer_service.h"

using namespace std::chrono_literals;

namespace {

struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> fired;

    void add(int v) {
        {
            std::lock_guard lock(m);
            fired.push_back(v);
        }
        cv.notify_all();
    }

    bool wait_for_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return fired.size() >= n; });
    }
};

}  // namespace

TEST(TimerService, FiresInDeadlineOrder) {
    lb::TimerService timers;
    timers.start();
    Recorder r;
    const auto now = lb::Clock::now();
    timers.schedule(now + 60ms, [&] { r.add(3); });
    timers.schedule(now + 20ms, [&] { r.add(1); });
    timers.schedule(now + 40ms, [&] { r.add(2); });
    ASSERT_TRUE(r.wait_for_count(3, 2000ms));
    EXPECT_EQ(r.fired, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(timers.pending(), 0u);
}

TEST(TimerService, DoesNotFireEarly) {
    lb::TimerService timers;
    timers.start();
    Recorder r;
    const auto scheduled = lb::Clock::now();
    lb::TimePoint fired_at{};
    timers.schedule(scheduled + 100ms, [&] {
        fired_at = lb::Clock::now();
        r.add(1);
    });
    ASSERT_TRUE(r.wait_for_count(1, 2000ms));
    EXPECT_GE(fired_at - scheduled, 100ms);
}

TEST(TimerService, CancelledTimerNeverFires) {
    lb::TimerService timers;
    timers.start();
    Recorder r;
    const auto id = timers.schedule(lb::Clock::now() + 50ms, [&] { r.add(1); });
    timers.schedule(lb::Clock::now() + 100ms, [&] { r.add(2); });
    EXPECT_TRUE(timers.cancel(id));
    EXPECT_FALSE(timers.cancel(id));
    ASSERT_TRUE(r.wait_for_count(1, 2000ms));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(r.fired, (std::vector<int>{2}));
}

TEST(TimerService, EarlierScheduleWakesTheThread) {
    lb::TimerService timers;
    timers.start();
    Recorder r;
    timers.schedule(lb::Clock::now() + 10s, [&] { r.add(9); });
    const auto t0 = lb::Clock::now();
    timers.schedule(lb::Clock::now() + 30ms, [&] { r.add(1); });
    ASSERT_TRUE(r.wait_for_count(1, 2000ms));
    EXPECT_LT(lb::Clock::now() - t0, 1s);
}

TEST(TimerService, StopDropsPendingTimers) {
    std::atomic<int> fired{0};
    {
        lb::TimerService timers;
        timers.start();
        timers.schedule(lb::Clock::now() + 10s, [&] { ++fired; });
        timers.stop();
        EXPECT_EQ(timers.pending(), 0u);
    }
    EXPECT_EQ(fired.load(), 0);
}
