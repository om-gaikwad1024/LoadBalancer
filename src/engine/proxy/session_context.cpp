#include "proxy/session_context.h"

#include "proxy/client_session.h"

namespace lb {

std::string_view to_string(TraceStep step) noexcept {
    switch (step) {
        case TraceStep::RequestReceived: return "request_received";
        case TraceStep::BackendSelected: return "backend_selected";
        case TraceStep::BackendConnected: return "backend_connected";
        case TraceStep::RequestForwarded: return "request_forwarded";
        case TraceStep::ResponseReceived: return "response_received";
        case TraceStep::ResponseCompleted: return "response_completed";
        case TraceStep::ErrorResponse: return "error_response";
        case TraceStep::Aborted: return "aborted";
        case TraceStep::TimedOut: return "timed_out";
    }
    return "unknown";
}

}  // namespace lb

namespace lb::proxy {

void SessionRegistry::add(const std::shared_ptr<ClientSession>& session) {
    std::lock_guard lock(mutex_);
    sessions_.emplace(session.get(), session);
}

void SessionRegistry::remove(ClientSession* session) noexcept {
    std::lock_guard lock(mutex_);
    sessions_.erase(session);
    if (sessions_.empty()) empty_.notify_all();
}

std::vector<std::shared_ptr<ClientSession>> SessionRegistry::snapshot() {
    std::vector<std::shared_ptr<ClientSession>> out;
    std::lock_guard lock(mutex_);
    out.reserve(sessions_.size());
    for (auto& [ptr, weak] : sessions_) {
        if (auto s = weak.lock()) out.push_back(std::move(s));
    }
    return out;
}

bool SessionRegistry::wait_empty(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return empty_.wait_for(lock, timeout, [this] { return sessions_.empty(); });
}

}  // namespace lb::proxy
