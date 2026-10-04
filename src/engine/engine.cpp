#include "engine.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "backend/registry.h"
#include "config/config_store.h"
#include "core/timer_service.h"
#include "health/health_checker.h"
#include "log/event_log.h"
#include "metrics/metrics.h"
#include "net/iocp.h"
#include "net/listener.h"
#include "net/winsock.h"
#include "proxy/client_session.h"
#include "proxy/session_context.h"

namespace lb {

std::string_view engine_version() noexcept { return LB_VERSION; }

namespace {

// A named background thread that runs `tick` every `interval` until stopped. Used for the
// plan V maintenance thread (idle pooled connections; later stale sticky/rate-limit
// entries) and for the dashboard snapshot publisher (plan IV.17).
class PeriodicThread {
public:
    ~PeriodicThread() { stop(); }

    void start(const wchar_t* name, std::chrono::milliseconds interval, std::function<void()> tick) {
        thread_ = std::thread([this, name, interval, tick = std::move(tick)] {
            ::SetThreadDescription(::GetCurrentThread(), name);
            std::unique_lock lock(mutex_);
            while (!stopping_) {
                if (wake_.wait_for(lock, interval, [this] { return stopping_; })) break;
                lock.unlock();
                tick();
                lock.lock();
            }
        });
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace

class EngineImpl final : public net::AcceptSink {
public:
    explicit EngineImpl(std::shared_ptr<const ConfigSnapshot> config) : config_(std::move(config)) {
        ctx_.port = &port_;
        ctx_.ext = &ext_;
        ctx_.config = &config_;
        ctx_.backends = &backends_;
        ctx_.timers = &timers_;
        ctx_.counters = &counters_;
        ctx_.registry = &registry_;
        ctx_.trace = &trace_;
        ctx_.stopping = &stopping_;
        ctx_.next_request_id = &next_request_id_;
        ctx_.request_ids = &request_ids_;
    }

    ~EngineImpl() { stop(); }

    bool start(std::string* error) {
        if (started_) {
            *error = "engine already started";
            return false;
        }
        const auto config = config_.current();
        if (!config) {
            *error = "no configuration (the config was rejected or never loaded)";
            return false;
        }
        backends_.load(*config);

        origin_ = Clock::now();
        events_ = std::make_unique<log::EventLog>(config->event_log, origin_);
        if (!events_->start(error)) return false;
        std::vector<std::string> ids;
        for (const auto& b : backends_.all()) ids.push_back(b->id);
        metrics_ = std::make_unique<metrics::Metrics>(std::move(ids), config->metrics, origin_);
        ctx_.metrics = metrics_.get();
        ctx_.events = events_.get();

        if (!winsock_.init(error) || !port_.create(error) || !ext_.load(error)) return false;

        const std::uint32_t threads =
            config->workers.threads ? *config->workers.threads
                                    : static_cast<std::uint32_t>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        workers_.start(port_, threads == 0 ? 1 : threads);
        timers_.start();
        maintenance_.start(L"lb-maintenance", std::chrono::milliseconds(config->maintenance.interval_ms),
                           [this] { backends_.sweep(Clock::now()); });
        started_ = true;

        // Plan IV.16: every health transition is logged with its reason and probe count.
        const auto on_transition = [this](const health::HealthTransition& t) {
            (t.up ? counters_.backends_marked_up : counters_.backends_marked_down).fetch_add(1);
            if (t.up) {
                events_->emit("backend_marked_healthy",
                              t.backend_id + " marked healthy after " + std::to_string(t.in_a_row) +
                                  " successful probes",
                              {{"successes", t.in_a_row}}, t.backend_id);
            } else {
                events_->emit("backend_marked_unhealthy",
                              t.backend_id + " marked unhealthy after " + std::to_string(t.in_a_row) +
                                  " failed probes: " + t.reason,
                              {{"failures", t.in_a_row}, {"reason", t.reason}}, t.backend_id);
            }
        };
        if (!health_.start(*config, backends_.all(), on_transition, error)) {
            stop();
            return false;
        }
        listener_ = std::make_unique<net::Listener>(port_, ext_, *this);
        if (!listener_->start(config->listen, error)) {
            stop();
            return false;
        }
        events_->emit("engine_started",
                      "listening on " + config->listen.address + ":" + std::to_string(listener_->port()) + " with " +
                          std::to_string(workers_.size()) + " worker threads",
                      {{"address", config->listen.address},
                       {"port", listener_->port()},
                       {"workers", workers_.size()},
                       {"backends", backends_.all().size()}});
        listen_address_ = config->listen.address;
        if (sink_.load() != nullptr) {
            publisher_.start(L"lb-dashboard-publisher", std::chrono::milliseconds(config->dashboard.publish_interval_ms),
                             [this] { publish(); });
        }
        return true;
    }

    void stop() {
        if (!started_ || stopped_) return;
        stopped_ = true;
        stopping_.store(true);
        publisher_.stop();  // the dashboard gets no further snapshots once shutdown begins
        health_.stop();

        // 1. Stop accepting. Pending AcceptEx calls complete and their sockets are closed.
        if (listener_) listener_->stop();

        // 2. Close idle connections now; busy ones close after their current response.
        for (auto& session : registry_.snapshot()) session->request_shutdown();

        // 3. Grace period for in-flight requests, then force-close whatever is left.
        const auto grace = std::chrono::milliseconds(config_.current()->timeouts.shutdown_grace_ms);
        if (!registry_.wait_empty(grace)) {
            for (auto& session : registry_.snapshot()) session->force_close();
            // Every socket is closed now, so each outstanding I/O completes promptly with an error.
            while (!registry_.wait_empty(std::chrono::milliseconds(1000))) {
                for (auto& session : registry_.snapshot()) session->force_close();
            }
        }

        // 4. No session is left: close pooled idle connections and stop background threads.
        maintenance_.stop();
        timers_.stop();
        backends_.close_all_idle();

        // 5. No I/O can be outstanding any more: stop the workers and release the port.
        workers_.stop();
        listener_.reset();
        port_.close();

        // 6. Last entry, then flush the log (plan VI: flush logs before exit).
        const EngineStats s = stats();
        events_->emit("engine_stopped",
                      "stopped after " + std::to_string(s.connections_accepted) + " connections and " +
                          std::to_string(s.requests_completed) + " requests",
                      {{"connections", s.connections_accepted}, {"requests", s.requests_completed}});
        events_->stop();
    }

    void on_accepted(SOCKET s, const sockaddr_in& peer) noexcept override {
        if (stopping_.load() || !port_.associate(s)) {
            ::closesocket(s);
            return;
        }
        net::set_no_delay(s);
        counters_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
        const auto limit = config_.current()->limits.max_client_connections;
        const bool reject = counters_.connections_active.load(std::memory_order_relaxed) >= limit;
        if (reject) {  // plan IV.1: refused and logged, never silently dropped
            try {
                const std::string peer_ip = proxy::format_ipv4(::ntohl(peer.sin_addr.s_addr));
                events_->emit("connection_rejected",
                              "connection from " + peer_ip + " refused with 503: " + std::to_string(limit) +
                                  " connections open (limits.max_client_connections)",
                              {{"peer", peer_ip}, {"limit", limit}});
            } catch (...) {
            }
        }
        try {
            auto session = std::make_shared<proxy::ClientSession>(ctx_, s, peer, reject);
            registry_.add(session);
            session->start();
        } catch (...) {
            ::closesocket(s);  // allocation failure: refuse this connection only
        }
    }

    std::uint16_t listen_port() const noexcept { return listener_ ? listener_->port() : 0; }
    std::uint32_t worker_threads() const noexcept { return workers_.size(); }

    EngineStats stats() const noexcept {
        EngineStats s;
        s.connections_accepted = counters_.connections_accepted.load();
        s.connections_active = counters_.connections_active.load();
        s.connections_rejected = counters_.connections_rejected.load();
        s.requests_completed = counters_.requests_completed.load();
        s.error_responses = counters_.error_responses.load();
        s.backend_connections_opened = counters_.backend_connections_opened.load();
        s.backend_connections_reused = counters_.backend_connections_reused.load();
        s.stale_retries = counters_.stale_retries.load();
        s.pool_rejections = counters_.pool_rejections.load();
        s.no_backend_available = counters_.no_backend_available.load();
        s.backends_marked_down = counters_.backends_marked_down.load();
        s.backends_marked_up = counters_.backends_marked_up.load();
        s.client_timeouts = counters_.client_timeouts.load();
        s.backend_timeouts = counters_.backend_timeouts.load();
        s.stale_retry_successes = counters_.stale_retry_successes.load();
        s.events_dropped = events_ ? events_->dropped() : 0;
        return s;
    }

    MetricsSnapshot metrics() const {
        return metrics_ ? metrics_->snapshot(Clock::now()) : MetricsSnapshot{};
    }

    std::vector<LoggedEvent> recent_events() const {
        return events_ ? events_->recent() : std::vector<LoggedEvent>{};
    }

    std::vector<BackendStats> backend_stats() const {
        std::vector<BackendStats> out;
        for (const auto& b : backends_.all()) out.push_back(b->stats());
        return out;
    }

    bool set_backend_state(std::string_view id, BackendState state) { return backends_.set_state(id, state); }

    void set_trace_sink(TraceSink* sink) noexcept { trace_.store(sink, std::memory_order_release); }
    void set_snapshot_sink(SnapshotSink* sink) noexcept { sink_.store(sink, std::memory_order_release); }

private:
    // Publisher thread (plan IV.17): a copied snapshot for the UI, plus the event-log
    // entries added since the previous one. The UI never reads engine state itself.
    void publish() noexcept {
        SnapshotSink* sink = sink_.load(std::memory_order_acquire);
        if (sink == nullptr) return;
        try {
            const TimePoint now = Clock::now();
            auto snap = std::make_unique<DashboardSnapshot>();
            snap->sequence = ++publish_sequence_;
            snap->listen_address = listen_address_;
            snap->listen_port = listen_port();
            snap->workers = workers_.size();
            snap->uptime_seconds = std::chrono::duration<double>(now - origin_).count();
            snap->stats = stats();
            snap->backends = backend_stats();
            snap->metrics = metrics_->snapshot(now);
            for (auto& e : events_->recent()) {
                if (e.sequence > last_published_event_) snap->new_events.push_back(std::move(e));
            }
            if (!snap->new_events.empty()) last_published_event_ = snap->new_events.back().sequence;
            sink->on_snapshot(std::move(snap));
        } catch (...) {
            // Allocation failure: skip this refresh; the next one carries the state.
        }
    }
    // Declaration order is teardown order in reverse: Winsock outlives every socket, the
    // registry (pools) outlives sessions, background threads stop before what they touch.
    ConfigStore config_;
    net::WinsockRuntime winsock_;
    net::CompletionPort port_;
    net::SocketExtensions ext_;
    backend::BackendRegistry backends_;
    TimerService timers_;
    PeriodicThread maintenance_;
    health::HealthChecker health_;
    net::WorkerPool workers_;
    std::unique_ptr<net::Listener> listener_;
    proxy::SessionRegistry registry_;
    proxy::EngineCounters counters_;
    proxy::SessionContext ctx_;
    std::atomic<TraceSink*> trace_{nullptr};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> next_request_id_{0};
    proxy::RequestIdGenerator request_ids_;
    TimePoint origin_{};
    std::unique_ptr<log::EventLog> events_;
    std::unique_ptr<metrics::Metrics> metrics_;
    std::atomic<SnapshotSink*> sink_{nullptr};
    std::string listen_address_;
    std::uint64_t publish_sequence_ = 0;      // publisher thread only
    std::uint64_t last_published_event_ = 0;  // publisher thread only
    PeriodicThread publisher_;  // declared last: stopped (and destroyed) first
    bool started_ = false;
    bool stopped_ = false;
};

Engine::Engine(std::shared_ptr<const ConfigSnapshot> config) : impl_(std::make_unique<EngineImpl>(std::move(config))) {}

Engine::~Engine() = default;

bool Engine::start(std::string* error) { return impl_->start(error); }
void Engine::stop() { impl_->stop(); }
std::uint16_t Engine::listen_port() const noexcept { return impl_->listen_port(); }
std::uint32_t Engine::worker_threads() const noexcept { return impl_->worker_threads(); }
EngineStats Engine::stats() const noexcept { return impl_->stats(); }
std::vector<BackendStats> Engine::backend_stats() const { return impl_->backend_stats(); }
MetricsSnapshot Engine::metrics() const { return impl_->metrics(); }
std::vector<LoggedEvent> Engine::recent_events() const { return impl_->recent_events(); }
bool Engine::set_backend_state(std::string_view backend_id, BackendState state) {
    return impl_->set_backend_state(backend_id, state);
}
void Engine::set_trace_sink(TraceSink* sink) noexcept { impl_->set_trace_sink(sink); }
void Engine::set_snapshot_sink(SnapshotSink* sink) noexcept { impl_->set_snapshot_sink(sink); }

}  // namespace lb
