#include "core/timer_service.h"

#include <windows.h>

namespace lb {

void TimerService::start() {
    std::lock_guard lock(mutex_);
    stopping_ = false;
    thread_ = std::thread([this] {
        ::SetThreadDescription(::GetCurrentThread(), L"lb-timer");
        run();
    });
}

void TimerService::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) return;
        stopping_ = true;
    }
    changed_.notify_all();
    thread_.join();
    std::lock_guard lock(mutex_);
    queue_.clear();
    index_.clear();
}

TimerService::Id TimerService::schedule(TimePoint when, std::function<void()> callback) {
    Id id;
    bool earliest;
    {
        std::lock_guard lock(mutex_);
        id = ++next_id_;
        const auto it = queue_.emplace(when, std::make_pair(id, std::move(callback)));
        index_.emplace(id, it);
        earliest = it == queue_.begin();
    }
    if (earliest) changed_.notify_one();
    return id;
}

bool TimerService::cancel(Id id) {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(id);
    if (it == index_.end()) return false;
    queue_.erase(it->second);
    index_.erase(it);
    return true;
}

std::size_t TimerService::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void TimerService::run() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (queue_.empty()) {
            changed_.wait(lock);
            continue;
        }
        const TimePoint next = queue_.begin()->first;
        if (Clock::now() < next) {
            changed_.wait_until(lock, next);
            continue;
        }
        auto callback = std::move(queue_.begin()->second.second);
        index_.erase(queue_.begin()->second.first);
        queue_.erase(queue_.begin());
        lock.unlock();
        callback();
        lock.lock();
    }
}

}  // namespace lb
