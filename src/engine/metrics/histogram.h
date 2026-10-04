#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "metrics/metrics_types.h"

namespace lb::metrics {

// HdrHistogram-style log-linear buckets over microseconds (plan IV.15): fixed memory,
// bounded relative error. Values below 128 us get exact buckets; above, every power of
// two is split into 64 buckets, so a bucket is at most 1/64 (1.6%) wide relative to
// its values. Values are capped at about 71 minutes.
class Buckets {
public:
    static constexpr std::uint64_t kMaxValue = (1ull << 32) - 1;
    static constexpr std::size_t kLinear = 128;
    static constexpr std::size_t kPerOctave = 64;
    static constexpr std::size_t kCount = kLinear + (32 - 7) * kPerOctave;  // 1728

    static std::size_t index(std::uint64_t value) noexcept {
        value = std::min(value, kMaxValue);
        if (value < kLinear) return static_cast<std::size_t>(value);
        const unsigned msb = static_cast<unsigned>(std::bit_width(value)) - 1;  // >= 7
        const unsigned octave = msb - 6;                                         // >= 1
        const auto sub = static_cast<std::size_t>((value >> octave) - kPerOctave);
        return kLinear + (octave - 1) * kPerOctave + sub;
    }

    // Highest value that falls into bucket `i` (what percentiles report: conservative).
    static std::uint64_t upper(std::size_t i) noexcept {
        if (i < kLinear) return i;
        const std::size_t octave = (i - kLinear) / kPerOctave + 1;
        const std::size_t sub = (i - kLinear) % kPerOctave;
        const std::uint64_t lower = static_cast<std::uint64_t>(kPerOctave + sub) << octave;
        return lower + (1ull << octave) - 1;
    }
};

// Plain merged counts, built on read.
struct HistogramData {
    std::vector<std::uint64_t> counts = std::vector<std::uint64_t>(Buckets::kCount, 0);
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint64_t max = 0;

    // Smallest bucket upper bound with at least ceil(p * count) values at or below it.
    std::uint64_t percentile(double p) const noexcept;
    LatencyStats stats() const noexcept;
};

// Written by exactly one thread (its owning worker), read by any. Buckets are atomics
// written with plain load+store (no lock prefix): a reader sees each counter whole.
template <typename CountT>
class LatencyHistogram {
public:
    void record(std::uint64_t micros) noexcept {
        bump(counts_[Buckets::index(micros)]);
        bump(count_);
        sum_.store(sum_.load(std::memory_order_relaxed) + micros, std::memory_order_relaxed);
        if (micros > max_.load(std::memory_order_relaxed)) max_.store(micros, std::memory_order_relaxed);
    }

    // Writer only (reuses a time slice).
    void clear() noexcept {
        for (auto& c : counts_) c.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        sum_.store(0, std::memory_order_relaxed);
        max_.store(0, std::memory_order_relaxed);
    }

    void add_to(HistogramData& out) const noexcept {
        if (count_.load(std::memory_order_relaxed) == 0) return;
        for (std::size_t i = 0; i < Buckets::kCount; ++i) out.counts[i] += counts_[i].load(std::memory_order_relaxed);
        out.count += count_.load(std::memory_order_relaxed);
        out.sum += sum_.load(std::memory_order_relaxed);
        out.max = std::max<std::uint64_t>(out.max, max_.load(std::memory_order_relaxed));
    }

private:
    template <typename T>
    static void bump(std::atomic<T>& c) noexcept {
        c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    std::array<std::atomic<CountT>, Buckets::kCount> counts_{};
    std::atomic<CountT> count_{0};
    std::atomic<std::uint64_t> sum_{0};
    std::atomic<std::uint64_t> max_{0};
};

}  // namespace lb::metrics
