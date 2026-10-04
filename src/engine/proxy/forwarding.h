#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "config/config.h"
#include "http/http_message.h"

namespace lb::proxy {

// What the proxy tells the backend about the original request (plan IV.6).
struct ForwardingContext {
    std::string_view client_ip;  // TCP peer address
    bool peer_trusted = false;   // TCP peer is in trusted_proxies
    std::string_view proto;      // "http" (phase 4 adds "https")
    std::string_view request_id;
    HostHeaderMode host_mode = HostHeaderMode::Preserve;
    std::string_view backend_endpoint;  // address:port, for Host rewriting and HTTP/1.0 without Host
};

std::string format_ipv4(std::uint32_t address_host_order);

// 1-128 characters of [A-Za-z0-9._-]: safe to log and to echo in a header.
bool valid_request_id(std::string_view id) noexcept;

// A well-formed X-Request-Id from a trusted upstream proxy is kept, so one id spans the
// whole chain; anything else is replaced by the proxy's own id (plan IV.6, VII).
std::string choose_request_id(const http::Fields& fields, bool peer_trusted, std::string_view generated);

// The address a client is identified by, e.g. for rate limiting (plan IV.6 trust rule).
// X-Forwarded-For is client-controlled and ignored unless the TCP peer is a trusted
// proxy; then the list is walked from the right, skipping trusted hops, and the first
// untrusted address is the client. Malformed entries stop the walk.
std::string client_identity(std::uint32_t peer_host_order, const http::Fields& fields, const ConfigSnapshot& config);

// Per-engine request id source: a random 64-bit prefix chosen at startup plus a counter,
// so ids are unique across restarts and cheap to make (32 hex characters).
class RequestIdGenerator {
public:
    RequestIdGenerator();
    std::string make(std::uint64_t sequence) const;

private:
    std::uint64_t prefix_;
};

}  // namespace lb::proxy
