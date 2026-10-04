#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "backend/backend_runtime.h"
#include "config/config.h"
#include "core/clock.h"
#include "health/hysteresis.h"

namespace lb::health {

struct HealthTransition {
    std::string backend_id;
    bool up = false;          // true: marked healthy again; false: marked unhealthy
    std::string reason;       // last probe's result, e.g. "connect refused", "HTTP 503", "timeout"
    std::uint32_t in_a_row = 0;  // consecutive probes that caused the change
};

// Active health checks (plan IV.10) on one dedicated timer-driven thread, isolated from
// live traffic (plan V). Probes are non-blocking sockets driven by WSAPoll, each with its
// own deadline, so a slow probe never delays the others.
class HealthChecker {
public:
    using Listener = std::function<void(const HealthTransition&)>;

    HealthChecker() = default;
    ~HealthChecker() { stop(); }
    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    // One probe target per backend, with its group's settings. The first probe of each
    // backend is spread over the first interval so probes do not arrive in a burst.
    bool start(const ConfigSnapshot& config, const std::vector<std::shared_ptr<backend::BackendRuntime>>& backends,
               Listener listener, std::string* error);
    void stop();

private:
    enum class Phase : std::uint8_t { Idle, Connecting, Sending, Receiving };

    struct Probe {
        std::shared_ptr<backend::BackendRuntime> backend;
        HealthConfig config;
        std::string request;  // HTTP probe request, empty for TCP
        Hysteresis hysteresis;
        TimePoint next_start{};
        TimePoint deadline{};
        Phase phase = Phase::Idle;
        SOCKET socket = INVALID_SOCKET;
        std::size_t sent = 0;
        std::string received;
    };

    void run();
    void begin(Probe& p, TimePoint now);
    void advance(Probe& p, short revents);
    void finish(Probe& p, bool ok, std::string detail);
    void wake() noexcept;

    std::vector<Probe> probes_;
    Listener listener_;
    SOCKET wake_socket_ = INVALID_SOCKET;  // loopback UDP: stop() sends a byte to interrupt WSAPoll
    sockaddr_in wake_address_{};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace lb::health
