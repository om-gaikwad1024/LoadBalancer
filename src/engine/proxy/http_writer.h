#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "http/http_message.h"

namespace lb::proxy {

// How the proxy frames a response body toward the client (plan IV.6: re-frame every body).
enum class ClientBodyMode : std::uint8_t {
    None,           // no body (HEAD, 1xx, 204, 304)
    ContentLength,  // same length as the backend sent
    Chunked,        // re-chunked (backend chunked or close-delimited, client HTTP/1.1)
    UntilClose,     // client is HTTP/1.0 and the length is unknown: close after the body
};

ClientBodyMode choose_client_body_mode(const http::ResponseHead& response, http::Version client_version) noexcept;

// Hop-by-hop fields (RFC 9110 7.6.1) plus framing fields, which the proxy always rewrites.
bool is_hop_by_hop(std::string_view name) noexcept;

// Request head for the backend: hop-by-hop fields, fields named in Connection, framing
// fields and Expect removed; framing re-added to match `request.framing`.
void append_backend_request_head(std::string& out, const http::RequestHead& request, std::string_view fallback_host);

void append_client_response_head(std::string& out, const http::ResponseHead& response, ClientBodyMode mode,
                                 bool keep_alive, http::Version client_version);

void append_chunk(std::string& out, std::string_view data);
void append_last_chunk(std::string& out);

// Complete proxy-generated response with Connection: close.
std::string error_response(int status);

std::string_view reason_phrase(int status) noexcept;

}  // namespace lb::proxy
