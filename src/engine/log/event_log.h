#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.h"
#include "core/clock.h"
#include "log/event_types.h"

namespace lb::log {

// Event log / audit trail (plan IV.16). emit() only formats the line and appends it to a
// lock-protected queue (plan V); a background thread writes the queue to disk and rotates
// files by size, so logging never blocks an IOCP worker on I/O. Each entry is one JSON
// object per line.
class EventLog {
public:
    EventLog(const EventLogConfig& config, TimePoint origin);
    ~EventLog();
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    // Opens the file (creating its directory) and starts the writer. No-op without a path.
    bool start(std::string* error);
    // Writes everything still queued, then joins the writer and closes the file.
    void stop();

    // fields: extra structured data; message: one human-readable line.
    void emit(std::string_view type, std::string message, nlohmann::json fields = nlohmann::json::object(),
              std::string_view backend = {}, std::string_view request_id = {});

    bool trace_requests() const noexcept { return config_.trace_requests; }
    std::vector<LoggedEvent> recent() const;
    std::uint64_t dropped() const;
    std::uint64_t write_errors() const;

private:
    void run();
    void write_line(const std::string& line);
    void rotate();
    bool open_file();

    const EventLogConfig config_;
    const TimePoint origin_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    std::deque<LoggedEvent> recent_;
    std::uint64_t sequence_ = 0;
    std::uint64_t dropped_ = 0;
    bool stopping_ = false;

    // Writer thread only.
    std::FILE* file_ = nullptr;
    std::uint64_t file_bytes_ = 0;
    std::uint64_t write_errors_ = 0;
    std::thread writer_;
};

}  // namespace lb::log
