#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config/config.h"
#include "core/clock.h"
#include "metrics/histogram.h"
#include "metrics/metrics_types.h"

namespace lb::metrics {

StatusClass status_class(int http_status) noexcept;

// Plan IV.15 / V: every IOCP worker records into its own ThreadRecorder (no lock, no
// shared cache line), and snapshot() merges all of them on read. Series 0 is the whole
// proxy; series i+1 is backend i (BackendRuntime::metrics_index).
class Metrics {
public:
    Metrics(std::vector<std::string> backend_ids, const MetricsConfig& config, TimePoint origin);
    ~Metrics();
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // One finished request. backend_series: 1-based backend series, or 0 if none was used.
    // backend_time is set only for responses that came from a backend.
    void record(std::size_t backend_series, TimePoint now, Duration total, std::optional<Duration> backend_time,
                StatusClass status) noexcept;

    MetricsSnapshot snapshot(TimePoint now) const;

private:
    struct Slice {
        std::atomic<std::int64_t> epoch{-1};
        LatencyHistogram<std::uint32_t> total;
        LatencyHistogram<std::uint32_t> backend;
        std::array<std::atomic<std::uint32_t>, kStatusClasses> status{};
    };

    struct Series {
        explicit Series(std::size_t slices) : window(slices) {}
        LatencyHistogram<std::uint64_t> total;
        LatencyHistogram<std::uint64_t> backend;
        std::array<std::atomic<std::uint64_t>, kStatusClasses> status{};
        std::vector<Slice> window;
    };

    struct ThreadRecorder {
        std::vector<std::unique_ptr<Series>> series;
    };

    ThreadRecorder& local();
    std::int64_t epoch_of(TimePoint t) const noexcept;
    void record_series(Series& s, std::int64_t epoch, std::uint64_t total_us, std::optional<std::uint64_t> backend_us,
                       StatusClass status) noexcept;

    const std::uint64_t instance_id_;  // tells this engine's thread_local recorders apart
    const std::vector<std::string> ids_;
    const MetricsConfig config_;
    const TimePoint origin_;

    mutable std::mutex recorders_mutex_;  // taken when a worker records for the first time, and by readers
    std::vector<std::unique_ptr<ThreadRecorder>> recorders_;
};

}  // namespace lb::metrics
