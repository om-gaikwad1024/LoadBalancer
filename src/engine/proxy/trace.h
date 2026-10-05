#pragma once

#include <cstdint>
#include <string_view>

namespace lb {

// Steps of the per-request pipeline (plan III, IV.18). The order a request passes
// through them is fixed; tests and (from step 1.9) the debug event log record it.
enum class TraceStep : std::uint8_t {
    RequestReceived,    // request head parsed
    GroupRouted,        // content routing chose the group (plan IV.8)
    BackendSelected,
    BackendConnected,
    RequestForwarded,   // whole request sent to the backend
    ResponseReceived,   // final response head parsed
    ResponseCompleted,  // whole response sent to the client
    ErrorResponse,      // proxy-generated error (status set)
    Aborted,            // client connection closed mid-response
    TimedOut,           // a plan VI timeout expired (status: the error sent, 0 if the connection was just closed)
};

std::string_view to_string(TraceStep step) noexcept;

struct TraceEvent {
    std::uint64_t request_id = 0;
    TraceStep step = TraceStep::RequestReceived;
    int status = 0;  // ResponseReceived / ResponseCompleted / ErrorResponse
};

// Called on IOCP worker threads: must be thread-safe and must not block.
class TraceSink {
public:
    virtual void on_trace(const TraceEvent& event) noexcept = 0;

protected:
    ~TraceSink() = default;
};

}  // namespace lb
