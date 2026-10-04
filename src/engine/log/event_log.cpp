#include "log/event_log.h"

#include <windows.h>
#include <share.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace lb::log {

namespace {

std::string utc_now_iso8601() {
    const auto now = std::chrono::system_clock::now();  // wall clock: display/correlation only (plan II.8)
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::gmtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

std::filesystem::path rotated(const std::filesystem::path& base, std::uint32_t n) {
    std::filesystem::path p = base;
    p += "." + std::to_string(n);
    return p;
}

}  // namespace

EventLog::EventLog(const EventLogConfig& config, TimePoint origin) : config_(config), origin_(origin) {}

EventLog::~EventLog() { stop(); }

bool EventLog::start(std::string* error) {
    if (config_.path.empty()) return true;
    if (!open_file()) {
        *error = "cannot open event log file: " + config_.path;
        return false;
    }
    writer_ = std::thread([this] {
        ::SetThreadDescription(::GetCurrentThread(), L"lb-event-log");
        run();
    });
    return true;
}

void EventLog::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool EventLog::open_file() {
    const std::filesystem::path path(config_.path);
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    file_ = ::_wfsopen(path.c_str(), L"ab", _SH_DENYWR);  // others may read (tail) while we write
    if (file_ == nullptr) return false;
    file_bytes_ = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    return true;
}

void EventLog::emit(std::string_view type, std::string message, nlohmann::json fields, std::string_view backend,
                    std::string_view request_id) {
    LoggedEvent e;
    e.wall_time = utc_now_iso8601();
    e.mono_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin_).count());
    e.type = std::string(type);
    e.backend = std::string(backend);
    e.request_id = std::string(request_id);
    e.message = std::move(message);

    nlohmann::json line = nlohmann::json::object();
    line["ts"] = e.wall_time;
    line["mono_ms"] = e.mono_ms;
    line["event"] = e.type;
    if (!e.backend.empty()) line["backend"] = e.backend;
    if (!e.request_id.empty()) line["request_id"] = e.request_id;
    line["message"] = e.message;
    if (fields.is_object()) {
        for (auto& [k, v] : fields.items()) line[k] = std::move(v);
    }

    std::lock_guard lock(mutex_);
    e.sequence = ++sequence_;
    line["seq"] = e.sequence;
    // Replace invalid UTF-8 instead of throwing: event text may quote peer-supplied bytes.
    e.json = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (!config_.path.empty() && !stopping_) {
        if (queue_.size() >= config_.max_queue) {
            ++dropped_;  // the disk cannot keep up: never block a worker, count the loss
        } else {
            queue_.push_back(e.json);
            wake_.notify_one();
        }
    }
    if (config_.recent_events > 0) {
        recent_.push_back(std::move(e));
        while (recent_.size() > config_.recent_events) recent_.pop_front();
    }
}

std::vector<LoggedEvent> EventLog::recent() const {
    std::lock_guard lock(mutex_);
    return {recent_.begin(), recent_.end()};
}

std::uint64_t EventLog::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

std::uint64_t EventLog::write_errors() const {
    std::lock_guard lock(mutex_);
    return write_errors_;
}

void EventLog::run() {
    std::vector<std::string> batch;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        batch.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
        queue_.clear();
        const bool last = stopping_;
        lock.unlock();
        for (const auto& line : batch) write_line(line);
        if (file_ != nullptr) std::fflush(file_);
        lock.lock();
        if (last && queue_.empty()) return;
    }
}

void EventLog::write_line(const std::string& line) {
    const std::uint64_t needed = line.size() + 1;
    if (file_bytes_ > 0 && file_bytes_ + needed > config_.max_file_bytes) rotate();
    if (file_ == nullptr) return;
    if (std::fwrite(line.data(), 1, line.size(), file_) != line.size() || std::fputc('\n', file_) == EOF) {
        std::lock_guard lock(mutex_);
        ++write_errors_;
        return;
    }
    file_bytes_ += needed;
}

// lb-events.jsonl -> lb-events.jsonl.1 -> ... -> lb-events.jsonl.<max_files>, oldest dropped.
void EventLog::rotate() {
    std::fclose(file_);
    file_ = nullptr;
    const std::filesystem::path base(config_.path);
    std::error_code ec;
    std::filesystem::remove(rotated(base, config_.max_files), ec);
    for (std::uint32_t i = config_.max_files; i > 1; --i) {
        std::filesystem::rename(rotated(base, i - 1), rotated(base, i), ec);
    }
    std::filesystem::rename(base, rotated(base, 1), ec);
    if (!open_file()) {
        std::lock_guard lock(mutex_);
        ++write_errors_;
    }
    file_bytes_ = 0;
}

}  // namespace lb::log
