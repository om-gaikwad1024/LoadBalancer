#include "metrics/metrics.h"

#include <chrono>
#include <cmath>

namespace lb::metrics {

namespace {

std::atomic<std::uint64_t> g_next_instance{1};

struct LocalRecorder {
    std::uint64_t owner = 0;
    void* recorder = nullptr;
};
thread_local LocalRecorder t_local;

std::uint64_t micros(Duration d) noexcept {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    return us < 0 ? 0 : static_cast<std::uint64_t>(us);
}

double to_ms(std::uint64_t us) noexcept { return static_cast<double>(us) / 1000.0; }

}  // namespace

std::uint64_t HistogramData::percentile(double p) const noexcept {
    if (count == 0) return 0;
    const auto rank = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(count)));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        seen += counts[i];
        if (seen >= std::max<std::uint64_t>(rank, 1)) return std::min(Buckets::upper(i), max);
    }
    return max;
}

LatencyStats HistogramData::stats() const noexcept {
    LatencyStats s;
    s.count = count;
    if (count == 0) return s;
    s.mean_ms = static_cast<double>(sum) / static_cast<double>(count) / 1000.0;
    s.p50_ms = to_ms(percentile(0.50));
    s.p95_ms = to_ms(percentile(0.95));
    s.p99_ms = to_ms(percentile(0.99));
    s.max_ms = to_ms(max);
    return s;
}

StatusClass status_class(int http_status) noexcept {
    if (http_status <= 0) return kAborted;
    if (http_status < 200) return k1xx;
    if (http_status < 300) return k2xx;
    if (http_status < 400) return k3xx;
    if (http_status < 500) return k4xx;
    return k5xx;
}

Metrics::Metrics(std::vector<std::string> backend_ids, const MetricsConfig& config, TimePoint origin)
    : instance_id_(g_next_instance.fetch_add(1)), ids_(std::move(backend_ids)), config_(config), origin_(origin) {}

Metrics::~Metrics() = default;

std::int64_t Metrics::epoch_of(TimePoint t) const noexcept {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - origin_).count();
    return ms < 0 ? 0 : ms / static_cast<std::int64_t>(config_.slice_ms);
}

Metrics::ThreadRecorder& Metrics::local() {
    if (t_local.owner == instance_id_) return *static_cast<ThreadRecorder*>(t_local.recorder);
    auto rec = std::make_unique<ThreadRecorder>();
    for (std::size_t i = 0; i <= ids_.size(); ++i) rec->series.push_back(std::make_unique<Series>(config_.window_slices));
    ThreadRecorder* raw = rec.get();
    {
        std::lock_guard lock(recorders_mutex_);
        recorders_.push_back(std::move(rec));
    }
    t_local = {instance_id_, raw};
    return *raw;
}

void Metrics::record_series(Series& s, std::int64_t epoch, std::uint64_t total_us,
                            std::optional<std::uint64_t> backend_us, StatusClass status) noexcept {
    s.total.record(total_us);
    if (backend_us) s.backend.record(*backend_us);
    s.status[status].store(s.status[status].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    // The slice for this moment; reused slices are cleared by their writer before use.
    Slice& slice = s.window[static_cast<std::size_t>(epoch % static_cast<std::int64_t>(s.window.size()))];
    if (slice.epoch.load(std::memory_order_relaxed) != epoch) {
        slice.total.clear();
        slice.backend.clear();
        for (auto& c : slice.status) c.store(0, std::memory_order_relaxed);
        slice.epoch.store(epoch, std::memory_order_release);
    }
    slice.total.record(total_us);
    if (backend_us) slice.backend.record(*backend_us);
    slice.status[status].store(slice.status[status].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void Metrics::record(std::size_t backend_series, TimePoint now, Duration total, std::optional<Duration> backend_time,
                     StatusClass status) noexcept {
    try {
        ThreadRecorder& rec = local();
        const std::int64_t epoch = epoch_of(now);
        const std::uint64_t total_us = micros(total);
        std::optional<std::uint64_t> backend_us;
        if (backend_time) backend_us = micros(*backend_time);
        record_series(*rec.series[0], epoch, total_us, backend_us, status);
        if (backend_series != 0 && backend_series < rec.series.size()) {
            record_series(*rec.series[backend_series], epoch, total_us, backend_us, status);
        }
    } catch (...) {
        // Allocation failure on a worker's first record: the sample is lost, the request is not.
    }
}

MetricsSnapshot Metrics::snapshot(TimePoint now) const {
    const std::int64_t current = epoch_of(now);
    const std::int64_t oldest = current - static_cast<std::int64_t>(config_.window_slices) + 1;

    const std::size_t n = ids_.size() + 1;
    std::vector<HistogramData> total_all(n), backend_all(n), total_win(n), backend_win(n);
    std::vector<std::array<std::uint64_t, kStatusClasses>> status_all(n), status_win(n);
    for (auto& a : status_all) a.fill(0);
    for (auto& a : status_win) a.fill(0);

    {
        std::lock_guard lock(recorders_mutex_);
        for (const auto& rec : recorders_) {
            for (std::size_t i = 0; i < n; ++i) {
                const Series& s = *rec->series[i];
                s.total.add_to(total_all[i]);
                s.backend.add_to(backend_all[i]);
                for (std::size_t c = 0; c < kStatusClasses; ++c) status_all[i][c] += s.status[c].load(std::memory_order_relaxed);
                for (const Slice& slice : s.window) {
                    const std::int64_t e = slice.epoch.load(std::memory_order_acquire);
                    if (e < oldest || e > current) continue;
                    slice.total.add_to(total_win[i]);
                    slice.backend.add_to(backend_win[i]);
                    for (std::size_t c = 0; c < kStatusClasses; ++c) {
                        status_win[i][c] += slice.status[c].load(std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    // The current slice is partly elapsed, so the window covers (slices - 1) full slices
    // plus the elapsed part (and never more than the time since start).
    const auto since_start = std::chrono::duration<double>(now - origin_).count();
    const double slice_s = config_.slice_ms / 1000.0;
    const double in_current = std::fmod(since_start, slice_s);
    const double covered = std::min(since_start, (config_.window_slices - 1) * slice_s + in_current);

    MetricsSnapshot out;
    out.window_seconds = config_.window_slices * slice_s;
    const auto fill = [&](std::size_t i, SeriesMetrics& m) {
        m.id = i == 0 ? "*" : ids_[i - 1];
        m.total_window = total_win[i].stats();
        m.total_since_start = total_all[i].stats();
        m.backend_window = backend_win[i].stats();
        m.backend_since_start = backend_all[i].stats();
        m.status_window = status_win[i];
        m.status_since_start = status_all[i];
        std::uint64_t requests = 0;
        for (auto c : status_win[i]) requests += c;
        m.requests_per_second = covered > 0 ? static_cast<double>(requests) / covered : 0;
        m.error_rate = requests > 0 ? static_cast<double>(status_win[i][k5xx] + status_win[i][kAborted]) /
                                          static_cast<double>(requests)
                                    : 0;
    };
    fill(0, out.system);
    for (std::size_t i = 1; i < n; ++i) {
        out.backends.emplace_back();
        fill(i, out.backends.back());
    }
    return out;
}

}  // namespace lb::metrics
