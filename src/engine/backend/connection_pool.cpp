#include "backend/connection_pool.h"

#include <algorithm>

namespace lb::backend {

namespace {

class SystemSocketOps final : public SocketOps {
public:
    bool is_stale(SOCKET s) noexcept override {
        WSAPOLLFD p{};
        p.fd = s;
        p.events = POLLRDNORM;
        const int rc = ::WSAPoll(&p, 1, 0);  // timeout 0: never blocks
        // Readable on an idle connection means FIN, RST or unsolicited bytes: unusable.
        return rc != 0;
    }

    void close(SOCKET s) noexcept override { ::closesocket(s); }
};

}  // namespace

SocketOps& system_socket_ops() noexcept {
    static SystemSocketOps ops;
    return ops;
}

ConnectionPool::Acquire ConnectionPool::acquire(SOCKET* out, const std::shared_ptr<PoolTicket>& ticket,
                                                bool prefer_new) {
    std::lock_guard lock(mutex_);
    const auto take_idle = [&]() -> bool {
        while (!idle_.empty()) {
            const SOCKET s = idle_.back().socket;
            idle_.pop_back();
            if (ops_.is_stale(s)) {  // the backend closed it while idle (plan IV.5, VI)
                ops_.close(s);
                --slots_;
                ++stale_discarded_;
                continue;
            }
            *out = s;
            ++reused_;
            return true;
        }
        return false;
    };

    if (!prefer_new && take_idle()) return Acquire::Reused;
    if (slots_ < limits_.max_connections) {
        ++slots_;
        ++opened_;
        return Acquire::Connect;
    }
    if (prefer_new && take_idle()) return Acquire::Reused;
    if (ticket && waiters_.size() < limits_.max_waiters) {
        waiters_.push_back(ticket);
        return Acquire::Queued;
    }
    return Acquire::Rejected;
}

void ConnectionPool::release(SOCKET s, bool reusable, TimePoint now) {
    std::vector<std::shared_ptr<PoolTicket>> wake;
    {
        std::lock_guard lock(mutex_);
        if (reusable) {
            // A waiter gets the connection directly; otherwise it goes idle if there is room.
            while (!waiters_.empty()) {
                auto t = std::move(waiters_.front());
                waiters_.pop_front();
                if (t->settled.exchange(true)) continue;  // timed out or cancelled meanwhile
                t->socket = s;
                ++reused_;
                wake.push_back(std::move(t));
                break;
            }
            if (wake.empty()) {
                if (idle_.size() < limits_.max_idle) {
                    idle_.push_back({s, now});
                } else {
                    ops_.close(s);
                    --slots_;
                }
            }
        } else {
            ops_.close(s);
            --slots_;
            grant_waiters_locked(&wake);
        }
    }
    for (auto& t : wake) t->wake();
}

void ConnectionPool::abandon_slot() {
    std::vector<std::shared_ptr<PoolTicket>> wake;
    {
        std::lock_guard lock(mutex_);
        --slots_;
        grant_waiters_locked(&wake);
    }
    for (auto& t : wake) t->wake();
}

bool ConnectionPool::cancel(const std::shared_ptr<PoolTicket>& ticket) {
    std::lock_guard lock(mutex_);
    if (ticket->settled.exchange(true)) return false;
    waiters_.erase(std::remove(waiters_.begin(), waiters_.end(), ticket), waiters_.end());
    return true;
}

void ConnectionPool::grant_waiters_locked(std::vector<std::shared_ptr<PoolTicket>>* wake) {
    while (slots_ < limits_.max_connections && !waiters_.empty()) {
        auto t = std::move(waiters_.front());
        waiters_.pop_front();
        if (t->settled.exchange(true)) continue;
        t->may_connect = true;
        ++slots_;
        ++opened_;
        wake->push_back(std::move(t));
    }
}

std::size_t ConnectionPool::close_idle() {
    std::vector<std::shared_ptr<PoolTicket>> wake;
    std::size_t closed = 0;
    {
        std::lock_guard lock(mutex_);
        for (const auto& c : idle_) ops_.close(c.socket);
        closed = idle_.size();
        slots_ -= static_cast<std::uint32_t>(closed);
        idle_.clear();
        grant_waiters_locked(&wake);
    }
    for (auto& t : wake) t->wake();
    return closed;
}

std::size_t ConnectionPool::close_expired(TimePoint now) {
    std::vector<std::shared_ptr<PoolTicket>> wake;
    std::size_t closed = 0;
    {
        std::lock_guard lock(mutex_);
        const auto expired = [&](const Idle& c) { return now - c.since >= limits_.idle_timeout; };
        for (const auto& c : idle_) {
            if (expired(c)) {
                ops_.close(c.socket);
                ++closed;
            }
        }
        idle_.erase(std::remove_if(idle_.begin(), idle_.end(), expired), idle_.end());
        slots_ -= static_cast<std::uint32_t>(closed);
        grant_waiters_locked(&wake);
    }
    for (auto& t : wake) t->wake();
    return closed;
}

PoolStats ConnectionPool::stats() const {
    std::lock_guard lock(mutex_);
    PoolStats s;
    s.open = slots_;
    s.idle = idle_.size();
    s.waiting = waiters_.size();
    s.opened = opened_;
    s.reused = reused_;
    s.stale_discarded = stale_discarded_;
    return s;
}

}  // namespace lb::backend
