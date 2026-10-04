#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "core/clock.h"

namespace lb::backend {

// Socket operations the pool needs, behind an interface so the pool logic is unit-testable.
class SocketOps {
public:
    // True if an idle connection can no longer be used: the backend closed it, reset it,
    // or sent bytes nobody asked for. Never blocks.
    virtual bool is_stale(SOCKET s) noexcept = 0;
    virtual void close(SOCKET s) noexcept = 0;

protected:
    ~SocketOps() = default;
};

SocketOps& system_socket_ops() noexcept;

// A request queued because its backend is at max_connections (plan IV.5). Settled exactly
// once, by whichever comes first: a free connection or slot, the wait timeout, or cancel.
struct PoolTicket {
    std::atomic<bool> settled{false};
    SOCKET socket = INVALID_SOCKET;  // granted: an idle connection to reuse
    bool may_connect = false;        // granted: a slot to open a new connection
    std::function<void()> wake;      // hands the outcome to the waiting request; set before queuing
};

struct PoolLimits {
    std::uint32_t max_connections = 0;
    std::uint32_t max_idle = 0;
    std::uint32_t max_waiters = 0;
    Duration idle_timeout{};
};

struct PoolStats {
    std::uint64_t open = 0;
    std::uint64_t idle = 0;
    std::uint64_t waiting = 0;
    std::uint64_t opened = 0;
    std::uint64_t reused = 0;
    std::uint64_t stale_discarded = 0;
};

// Keep-alive connections to one backend, guarded by one lock per backend (plan V).
// A "slot" counts every connection that is open, being opened, in use or idle, so the
// total never exceeds max_connections.
class ConnectionPool {
public:
    enum class Acquire { Reused, Connect, Queued, Rejected };

    ConnectionPool(const PoolLimits& limits, SocketOps& ops) noexcept : limits_(limits), ops_(ops) {}
    ~ConnectionPool() { close_idle(); }
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Reused: *out is a live idle connection. Connect: a slot is reserved, open a new one.
    // Queued: `ticket` waits for a slot. Rejected: cap reached and the wait queue is full.
    // prefer_new skips idle connections while a new slot is available (stale-connection retry).
    Acquire acquire(SOCKET* out, const std::shared_ptr<PoolTicket>& ticket, bool prefer_new = false);

    // Gives back a connection obtained from this pool. Reusable connections go to a waiter
    // or the idle list; others are closed and their slot freed.
    void release(SOCKET s, bool reusable, TimePoint now);
    // Frees a slot that never produced a usable connection (connect failed).
    void abandon_slot();
    // Removes a queued ticket. True if this call settled it (the caller must then wake it).
    bool cancel(const std::shared_ptr<PoolTicket>& ticket);

    // Backend went unhealthy or started draining, or the engine stops (plan IV.5).
    std::size_t close_idle();
    // Maintenance: closes connections idle for longer than idle_timeout.
    std::size_t close_expired(TimePoint now);

    // Hot reload (plan IV.14): new limits for later acquires and releases. Idle connections
    // over the new max_idle are closed (returns how many); queued tickets beyond a smaller
    // max_waiters keep waiting. Busy connections are never cut.
    std::size_t set_limits(const PoolLimits& limits);

    PoolStats stats() const;

private:
    struct Idle {
        SOCKET socket;
        TimePoint since;
    };

    // Hands freed capacity to queued tickets; returns those to wake (outside the lock).
    void grant_waiters_locked(std::vector<std::shared_ptr<PoolTicket>>* wake);

    PoolLimits limits_;
    SocketOps& ops_;
    mutable std::mutex mutex_;
    std::vector<Idle> idle_;  // most recently used at the back
    std::deque<std::shared_ptr<PoolTicket>> waiters_;
    std::uint32_t slots_ = 0;
    std::uint64_t opened_ = 0;
    std::uint64_t reused_ = 0;
    std::uint64_t stale_discarded_ = 0;
};

}  // namespace lb::backend
