#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

#include "metrics/histogram.h"
#include "metrics/metrics.h"

using lb::metrics::Buckets;
using lb::metrics::HistogramData;
using lb::metrics::LatencyHistogram;
using lb::metrics::Metrics;
using namespace std::chrono_literals;

// ---- Buckets: fixed memory, bounded relative error (plan IV.15) ------------------------

TEST(HistogramBuckets, SmallValuesAreExact) {
    for (std::uint64_t v = 0; v < 128; ++v) {
        EXPECT_EQ(Buckets::index(v), v);
        EXPECT_EQ(Buckets::upper(Buckets::index(v)), v);
    }
}

TEST(HistogramBuckets, RelativeErrorIsAtMostOneSixtyFourth) {
    std::mt19937_64 rng(42);
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t v = 128 + rng() % Buckets::kMaxValue;
        const std::uint64_t up = Buckets::upper(Buckets::index(v));
        ASSERT_GE(up, v);
        ASSERT_LE(static_cast<double>(up - v) / static_cast<double>(v), 1.0 / 64.0) << v;
    }
}

TEST(HistogramBuckets, BucketsAreContiguousAndCoverTheRange) {
    for (std::size_t i = 0; i + 1 < Buckets::kCount; ++i) {
        ASSERT_EQ(Buckets::index(Buckets::upper(i)), i);
        ASSERT_EQ(Buckets::index(Buckets::upper(i) + 1), i + 1);
    }
    EXPECT_EQ(Buckets::index(Buckets::kMaxValue), Buckets::kCount - 1);
    EXPECT_EQ(Buckets::index(~0ull), Buckets::kCount - 1);  // capped, never out of range
}

TEST(Histogram, PercentilesAreWithinTheBucketError) {
    LatencyHistogram<std::uint64_t> h;
    for (std::uint64_t v = 1; v <= 100000; ++v) h.record(v);
    HistogramData d;
    h.add_to(d);
    const auto s = d.stats();
    EXPECT_EQ(s.count, 100000u);
    EXPECT_NEAR(s.p50_ms, 50.0, 50.0 / 64);
    EXPECT_NEAR(s.p95_ms, 95.0, 95.0 / 64);
    EXPECT_NEAR(s.p99_ms, 99.0, 99.0 / 64);
    EXPECT_DOUBLE_EQ(s.max_ms, 100.0);           // exact
    EXPECT_NEAR(s.mean_ms, 50.0005, 1e-9);       // exact (sum / count)
    EXPECT_GE(s.p99_ms, 99.0);                   // reported conservatively (bucket upper bound)
}

TEST(Histogram, TailDominatesPercentilesNotTheMean) {
    LatencyHistogram<std::uint64_t> h;
    for (int i = 0; i < 980; ++i) h.record(1000);     // 1 ms
    for (int i = 0; i < 20; ++i) h.record(200000);    // 200 ms tail: 2%
    HistogramData d;
    h.add_to(d);
    const auto s = d.stats();
    EXPECT_LT(s.p50_ms, 1.1);
    EXPECT_GT(s.p99_ms, 190.0);
    EXPECT_DOUBLE_EQ(s.max_ms, 200.0);
    EXPECT_NEAR(s.mean_ms, 4.98, 0.01);  // the average hides the tail
}

TEST(Histogram, ClearAndEmpty) {
    LatencyHistogram<std::uint32_t> h;
    h.record(5);
    h.clear();
    HistogramData d;
    h.add_to(d);
    EXPECT_EQ(d.count, 0u);
    EXPECT_EQ(d.stats().p99_ms, 0.0);
}

// ---- Metrics: per-thread recorders, rolling window ------------------------------------

namespace {

lb::MetricsConfig window_config() {
    lb::MetricsConfig c;
    c.slice_ms = 1000;
    c.window_slices = 10;
    return c;
}

const lb::TimePoint origin = lb::Clock::now();

}  // namespace

TEST(Metrics, RecordsSystemAndBackendSeries) {
    Metrics m({"b1", "b2"}, window_config(), origin);
    m.record(1, origin + 100ms, 5ms, 4ms, lb::k2xx);
    m.record(2, origin + 200ms, 7ms, 6ms, lb::k5xx);
    m.record(0, origin + 300ms, 1ms, std::nullopt, lb::k4xx);  // proxy-generated: no backend

    const auto s = m.snapshot(origin + 500ms);
    EXPECT_EQ(s.system.total_since_start.count, 3u);
    EXPECT_EQ(s.system.backend_since_start.count, 2u);  // the 4xx never reached a backend
    ASSERT_EQ(s.backends.size(), 2u);
    EXPECT_EQ(s.backends[0].id, "b1");
    EXPECT_EQ(s.backends[0].total_since_start.count, 1u);
    EXPECT_EQ(s.backends[0].status_since_start[lb::k2xx], 1u);
    EXPECT_EQ(s.backends[1].status_since_start[lb::k5xx], 1u);
    EXPECT_EQ(s.system.status_window[lb::k4xx], 1u);
    EXPECT_DOUBLE_EQ(s.backends[1].error_rate, 1.0);
    EXPECT_NEAR(s.system.error_rate, 1.0 / 3.0, 1e-9);
}

TEST(Metrics, LiveWindowForgetsOldSamplesButSinceStartKeepsThem) {
    Metrics m({"b1"}, window_config(), origin);
    m.record(1, origin + 500ms, 10ms, 9ms, lb::k2xx);
    auto s = m.snapshot(origin + 600ms);
    EXPECT_EQ(s.system.total_window.count, 1u);

    s = m.snapshot(origin + 20s);  // window is 10 s
    EXPECT_EQ(s.system.total_window.count, 0u);
    EXPECT_EQ(s.system.total_since_start.count, 1u);
    EXPECT_DOUBLE_EQ(s.window_seconds, 10.0);
}

TEST(Metrics, AReusedSliceStartsEmpty) {
    Metrics m({"b1"}, window_config(), origin);
    m.record(1, origin + 500ms, 10ms, 9ms, lb::k2xx);    // slice 0, epoch 0
    m.record(1, origin + 10500ms, 20ms, 19ms, lb::k2xx);  // slice 0 again, epoch 10
    const auto s = m.snapshot(origin + 10600ms);
    EXPECT_EQ(s.system.total_window.count, 1u);
    EXPECT_NEAR(s.system.total_window.max_ms, 20.0, 0.01);
    EXPECT_EQ(s.system.total_since_start.count, 2u);
}

TEST(Metrics, RequestRateIsOverTheCoveredWindow) {
    Metrics m({"b1"}, window_config(), origin);
    for (int i = 0; i < 50; ++i) m.record(1, origin + std::chrono::milliseconds(100 * i), 1ms, 1ms, lb::k2xx);
    const auto s = m.snapshot(origin + 5s);  // 50 requests in 5 s
    EXPECT_NEAR(s.system.requests_per_second, 10.0, 0.01);
}

TEST(Metrics, EveryThreadRecordsIntoItsOwnRecorderWithoutLoss) {
    Metrics m({"b1"}, window_config(), origin);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10000; ++i) m.record(1, lb::Clock::now(), 1ms, 1ms, lb::k2xx);
        });
    }
    // Reads run concurrently with the writers.
    for (int i = 0; i < 20; ++i) (void)m.snapshot(lb::Clock::now());
    for (auto& t : threads) t.join();
    const auto s = m.snapshot(lb::Clock::now());
    EXPECT_EQ(s.system.total_since_start.count, 80000u);
    EXPECT_EQ(s.backends[0].status_since_start[lb::k2xx], 80000u);
}

TEST(Metrics, StatusClassMapping) {
    EXPECT_EQ(lb::metrics::status_class(0), lb::kAborted);
    EXPECT_EQ(lb::metrics::status_class(101), lb::k1xx);
    EXPECT_EQ(lb::metrics::status_class(204), lb::k2xx);
    EXPECT_EQ(lb::metrics::status_class(304), lb::k3xx);
    EXPECT_EQ(lb::metrics::status_class(431), lb::k4xx);
    EXPECT_EQ(lb::metrics::status_class(504), lb::k5xx);
}
