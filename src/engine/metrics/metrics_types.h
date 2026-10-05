#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lb {

// Percentiles of one latency series (plan IV.15: never just an average).
struct LatencyStats {
    std::uint64_t count = 0;
    double mean_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;  // exact
};

// Response outcomes by class: 1xx, 2xx, 3xx, 4xx, 5xx, and aborted (the client got an
// incomplete response because the backend broke mid-body or the connection was cut).
enum StatusClass : std::size_t { k1xx, k2xx, k3xx, k4xx, k5xx, kAborted, kStatusClasses };

// One completed time slice of a series, for the dashboard's graphs (plan IV.17).
struct SlicePoint {
    std::int64_t slice = 0;  // slice number since engine start: it covers [slice, slice + 1) x slice_seconds
    std::uint64_t requests = 0;
    std::uint64_t errors = 0;  // 5xx + aborted
    double p50_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
};

struct SeriesMetrics {
    std::string id;  // backend id, or "*" for the whole proxy
    // Total time at the proxy (first request byte in to last response byte out) and
    // backend time alone (backend connection ready to response fully received), so
    // proxy overhead is visible.
    LatencyStats total_window;
    LatencyStats total_since_start;
    LatencyStats backend_window;
    LatencyStats backend_since_start;
    std::array<std::uint64_t, kStatusClasses> status_window{};
    std::array<std::uint64_t, kStatusClasses> status_since_start{};
    double requests_per_second = 0;  // over the live window
    double error_rate = 0;           // (5xx + aborted) / requests, over the live window
    // The live window's completed slices, oldest first, including empty ones (no traffic).
    std::vector<SlicePoint> slices;
};

// Copied, merged view of every worker's histograms; safe to hand to the UI thread.
struct MetricsSnapshot {
    double window_seconds = 0;
    double slice_seconds = 0;
    SeriesMetrics system;
    std::vector<SeriesMetrics> backends;
};

}  // namespace lb
