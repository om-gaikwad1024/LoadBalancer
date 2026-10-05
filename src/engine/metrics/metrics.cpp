#include "metrics/metrics.h"

#include <algorithm>
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
    : instance_id_(g_next_instance.fetch_add(1)),
      capacity_(std::max<std::size_t>(config.max_backend_series, backend_ids.size())),
      ids_(std::move(backend_ids)),
      config_(config),
      origin_(origin) {}

Metrics::~Metrics() = default;

std::size_t Metrics::register_series(const std::string& id) {
    std::lock_guard lock(ids_mutex_);
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        if (ids_[i] == id) return i + 1;
    }
    if (ids_.size() >= capacity_) return 0;
    ids_.push_back(id);
    return ids_.size();
}

bool Metrics::has_room_for(const std::vector<std::string>& ids) const {
    std::lock_guard lock(ids_mutex_);
    std::size_t fresh = 0;
    for (const auto& id : ids) {
        if (std::find(ids_.begin(), ids_.end(), id) == ids_.end()) ++fresh;
    }
    return ids_.size() + fresh <= capacity_;
}

Metrics::Series& Metrics::ThreadRecorder::at(std::size_t i, std::size_t slices) {
    Series* s = series[i].load(std::memory_order_acquire);
    if (s == nullptr) {
        s = new Series(slices);
        series[i].store(s, std::memory_order_release);
    }
    return *s;
}

std::int64_t Metrics::epoch_of(TimePoint t) const noexcept {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - origin_).count();
    return ms < 0 ? 0 : ms / static_cast<std::int64_t>(config_.slice_ms);
}

Metrics::ThreadRecorder& Metrics::local() {
    if (t_local.owner == instance_id_) return *static_cast<ThreadRecorder*>(t_local.recorder);
    auto rec = std::make_unique<ThreadRecorder>(capacity_ + 1);
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
        record_series(rec.at(0, config_.window_slices), epoch, total_us, backend_us, status);
        if (backend_series != 0 && backend_series < rec.count) {
            record_series(rec.at(backend_series, config_.window_slices), epoch, total_us, backend_us, status);
        }
    } catch (...) {
        // Allocation failure on a worker's first record: the sample is lost, the request is not.
    }
}

MetricsSnapshot Metrics::snapshot(TimePoint now) const {
    const std::int64_t current = epoch_of(now);
    const std::int64_t oldest = current - static_cast<std::int64_t>(config_.window_slices) + 1;

    std::vector<std::string> ids;
    {
        std::lock_guard lock(ids_mutex_);
        ids = ids_;
    }
    const std::size_t n = ids.size() + 1;
    std::vector<HistogramData> total_all(n), backend_all(n), total_win(n), backend_win(n);
    std::vector<std::array<std::uint64_t, kStatusClasses>> status_all(n), status_win(n);
    for (auto& a : status_all) a.fill(0);
    for (auto& a : status_win) a.fill(0);
    // Per completed slice of the window, for the graphs: index = epoch - oldest.
    const std::size_t slices = config_.window_slices;
    std::vector<std::vector<HistogramData>> slice_total(n, std::vector<HistogramData>(slices));
    std::vector<std::vector<std::array<std::uint64_t, kStatusClasses>>> slice_status(
        n, std::vector<std::array<std::uint64_t, kStatusClasses>>(slices, std::array<std::uint64_t, kStatusClasses>{}));

    {
        std::lock_guard lock(recorders_mutex_);
        for (const auto& rec : recorders_) {
            for (std::size_t i = 0; i < n && i < rec->count; ++i) {
                const Series* sp = rec->series[i].load(std::memory_order_acquire);
                if (sp == nullptr) continue;  // this thread never recorded for that series
                const Series& s = *sp;
                s.total.add_to(total_all[i]);
                s.backend.add_to(backend_all[i]);
                for (std::size_t c = 0; c < kStatusClasses; ++c) status_all[i][c] += s.status[c].load(std::memory_order_relaxed);
                for (const Slice& slice : s.window) {
                    const std::int64_t e = slice.epoch.load(std::memory_order_acquire);
                    if (e < oldest || e > current) continue;
                    slice.total.add_to(total_win[i]);
                    slice.backend.add_to(backend_win[i]);
                    const auto k = static_cast<std::size_t>(e - oldest);
                    slice.total.add_to(slice_total[i][k]);
                    for (std::size_t c = 0; c < kStatusClasses; ++c) {
                        const auto v = slice.status[c].load(std::memory_order_relaxed);
                        status_win[i][c] += v;
                        slice_status[i][k][c] += v;
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
    out.slice_seconds = slice_s;
    const auto fill = [&](std::size_t i, SeriesMetrics& m) {
        m.id = i == 0 ? "*" : ids[i - 1];
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
        // Completed slices only (the current one is still filling), never before the start.
        for (std::size_t k = 0; k < slices; ++k) {
            const std::int64_t epoch = oldest + static_cast<std::int64_t>(k);
            if (epoch < 0 || epoch >= current) continue;
            SlicePoint p;
            p.slice = epoch;
            for (auto c : slice_status[i][k]) p.requests += c;
            p.errors = slice_status[i][k][k5xx] + slice_status[i][k][kAborted];
            const LatencyStats st = slice_total[i][k].stats();
            p.p50_ms = st.p50_ms;
            p.p99_ms = st.p99_ms;
            p.max_ms = st.max_ms;
            m.slices.push_back(p);
        }
    };
    fill(0, out.system);
    for (std::size_t i = 1; i < n; ++i) {
        out.backends.emplace_back();
        fill(i, out.backends.back());
    }
    return out;
}

}  // namespace lb::metrics
