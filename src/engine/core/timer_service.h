#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/clock.h"

namespace lb {

// Deadline timer on one dedicated thread, using the monotonic clock (plan II.8).
// IOCP has no per-operation timeouts, so deadlines live here. Callbacks run on the timer
// thread and must be short and non-blocking: they hand work back to an IOCP worker
// (for example by posting a completion) instead of doing it themselves.
class TimerService {
public:
    using Id = std::uint64_t;

    TimerService() = default;
    ~TimerService() { stop(); }
    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    void start();
    // Joins the thread; callbacks that have not fired are dropped.
    void stop();

    Id schedule(TimePoint when, std::function<void()> callback);
    // Returns true if the timer had not fired yet (and now never will).
    bool cancel(Id id);

    std::size_t pending() const;

private:
    void run();

    using Queue = std::multimap<TimePoint, std::pair<Id, std::function<void()>>>;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Queue queue_;
    std::unordered_map<Id, Queue::iterator> index_;
    Id next_id_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace lb
