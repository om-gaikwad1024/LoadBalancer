#include "engine.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "backend/registry.h"
#include "config/config_store.h"
#include "core/timer_service.h"
#include "net/iocp.h"
#include "net/listener.h"
#include "net/winsock.h"
#include "proxy/client_session.h"
#include "proxy/session_context.h"

namespace lb {

std::string_view engine_version() noexcept { return LB_VERSION; }

namespace {

// Plan V maintenance thread: periodic housekeeping off the request path. Step 1.5: closes
// pooled connections past their idle timeout (later: stale sticky and rate-limit entries).
class MaintenanceThread {
public:
    ~MaintenanceThread() { stop(); }

    void start(std::chrono::milliseconds interval, backend::BackendRegistry& backends) {
        thread_ = std::thread([this, interval, &backends] {
            ::SetThreadDescription(::GetCurrentThread(), L"lb-maintenance");
            std::unique_lock lock(mutex_);
            while (!stopping_) {
                if (wake_.wait_for(lock, interval, [this] { return stopping_; })) break;
                lock.unlock();
                backends.sweep(Clock::now());
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
        if (!winsock_.init(error) || !port_.create(error) || !ext_.load(error)) return false;

        const std::uint32_t threads =
            config->workers.threads ? *config->workers.threads
                                    : static_cast<std::uint32_t>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        workers_.start(port_, threads == 0 ? 1 : threads);
        timers_.start();
        maintenance_.start(std::chrono::milliseconds(config->maintenance.interval_ms), backends_);
        started_ = true;

        listener_ = std::make_unique<net::Listener>(port_, ext_, *this);
        if (!listener_->start(config->listen, error)) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        if (!started_ || stopped_) return;
        stopped_ = true;
        stopping_.store(true);

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
    }

    void on_accepted(SOCKET s, const sockaddr_in& peer) noexcept override {
        if (stopping_.load() || !port_.associate(s)) {
            ::closesocket(s);
            return;
        }
        net::set_no_delay(s);
        counters_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
        const bool reject = counters_.connections_active.load(std::memory_order_relaxed) >=
                            config_.current()->limits.max_client_connections;
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
        s.client_timeouts = counters_.client_timeouts.load();
        s.backend_timeouts = counters_.backend_timeouts.load();
        return s;
    }

    std::vector<BackendStats> backend_stats() const {
        std::vector<BackendStats> out;
        for (const auto& b : backends_.all()) out.push_back(b->stats());
        return out;
    }

    bool set_backend_state(std::string_view id, BackendState state) { return backends_.set_state(id, state); }

    void set_trace_sink(TraceSink* sink) noexcept { trace_.store(sink, std::memory_order_release); }

private:
    // Declaration order is teardown order in reverse: Winsock outlives every socket, the
    // registry (pools) outlives sessions, background threads stop before what they touch.
    ConfigStore config_;
    net::WinsockRuntime winsock_;
    net::CompletionPort port_;
    net::SocketExtensions ext_;
    backend::BackendRegistry backends_;
    TimerService timers_;
    MaintenanceThread maintenance_;
    net::WorkerPool workers_;
    std::unique_ptr<net::Listener> listener_;
    proxy::SessionRegistry registry_;
    proxy::EngineCounters counters_;
    proxy::SessionContext ctx_;
    std::atomic<TraceSink*> trace_{nullptr};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> next_request_id_{0};
    proxy::RequestIdGenerator request_ids_;
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
bool Engine::set_backend_state(std::string_view backend_id, BackendState state) {
    return impl_->set_backend_state(backend_id, state);
}
void Engine::set_trace_sink(TraceSink* sink) noexcept { impl_->set_trace_sink(sink); }

}  // namespace lb
