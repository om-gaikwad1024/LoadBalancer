#include "proxy/http_writer.h"

#include <array>
#include <cstdio>
#include <vector>

namespace lb::proxy {

using http::iequals;

namespace {

constexpr std::array<std::string_view, 8> kDroppedFields = {
    "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Upgrade", "Content-Length", "Transfer-Encoding",
};

// Field names listed in the Connection header(s) are hop-by-hop too.
std::vector<std::string_view> connection_options(const http::Fields& fields) {
    std::vector<std::string_view> names;
    for (const auto& f : fields.all()) {
        if (!iequals(f.name, "Connection")) continue;
        std::string_view v = f.value;
        while (!v.empty()) {
            const auto comma = v.find(',');
            std::string_view token = v.substr(0, comma);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
            if (!token.empty()) names.push_back(token);
            if (comma == std::string_view::npos) break;
            v.remove_prefix(comma + 1);
        }
    }
    return names;
}

bool named_in(const std::vector<std::string_view>& names, std::string_view name) {
    for (auto n : names) {
        if (iequals(n, name)) return true;
    }
    return false;
}

void append_field(std::string& out, std::string_view name, std::string_view value) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
}

}  // namespace

bool is_hop_by_hop(std::string_view name) noexcept {
    for (auto f : kDroppedFields) {
        if (iequals(f, name)) return true;
    }
    return false;
}

ClientBodyMode choose_client_body_mode(const http::ResponseHead& response, http::Version client_version) noexcept {
    switch (response.framing) {
        case http::BodyFraming::None:
            return ClientBodyMode::None;
        case http::BodyFraming::ContentLength:
            return ClientBodyMode::ContentLength;
        case http::BodyFraming::Chunked:
        case http::BodyFraming::UntilClose:
            return client_version == http::Version::Http11 ? ClientBodyMode::Chunked : ClientBodyMode::UntilClose;
    }
    return ClientBodyMode::UntilClose;
}

void append_backend_request_head(std::string& out, const http::RequestHead& request, std::string_view fallback_host) {
    out.append(request.method);
    out.push_back(' ');
    out.append(request.target);
    out.append(" HTTP/1.1\r\n");

    const auto options = connection_options(request.fields);
    bool has_host = false;
    for (const auto& f : request.fields.all()) {
        if (is_hop_by_hop(f.name) || named_in(options, f.name) || iequals(f.name, "Expect")) continue;
        has_host = has_host || iequals(f.name, "Host");
        append_field(out, f.name, f.value);
    }
    if (!has_host) append_field(out, "Host", fallback_host);  // HTTP/1.0 client without Host

    if (request.framing == http::BodyFraming::Chunked) {
        append_field(out, "Transfer-Encoding", "chunked");
    } else if (request.framing == http::BodyFraming::ContentLength) {
        append_field(out, "Content-Length", std::to_string(request.content_length));
    }
    // Step 1.4: one backend connection per request. Step 1.5 (connection pool) keeps it alive.
    append_field(out, "Connection", "close");
    out.append("\r\n");
}

void append_client_response_head(std::string& out, const http::ResponseHead& response, ClientBodyMode mode,
                                 bool keep_alive, http::Version client_version) {
    out.append("HTTP/1.1 ");
    out.append(std::to_string(response.status));
    out.push_back(' ');
    out.append(response.reason.empty() ? reason_phrase(response.status) : std::string_view(response.reason));
    out.append("\r\n");

    const auto options = connection_options(response.fields);
    for (const auto& f : response.fields.all()) {
        // Without a body, Content-Length still describes the entity (HEAD, 304) and is kept.
        if (mode == ClientBodyMode::None && iequals(f.name, "Content-Length")) {
            append_field(out, f.name, f.value);
            continue;
        }
        if (is_hop_by_hop(f.name) || named_in(options, f.name)) continue;
        append_field(out, f.name, f.value);
    }

    if (mode == ClientBodyMode::ContentLength) {
        append_field(out, "Content-Length", std::to_string(response.content_length));
    } else if (mode == ClientBodyMode::Chunked) {
        append_field(out, "Transfer-Encoding", "chunked");
    }
    if (!keep_alive) {
        append_field(out, "Connection", "close");
    } else if (client_version == http::Version::Http10) {
        append_field(out, "Connection", "keep-alive");
    }
    out.append("\r\n");
}

void append_chunk(std::string& out, std::string_view data) {
    if (data.empty()) return;  // an empty chunk would end the body
    char size[20];
    const int n = std::snprintf(size, sizeof(size), "%zx\r\n", data.size());
    out.append(size, static_cast<std::size_t>(n));
    out.append(data);
    out.append("\r\n");
}

void append_last_chunk(std::string& out) { out.append("0\r\n\r\n"); }

std::string error_response(int status) {
    const std::string_view reason = reason_phrase(status);
    std::string body(reason);
    body.push_back('\n');
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
    out += "Content-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    out += body;
    return out;
}

std::string_view reason_phrase(int status) noexcept {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

}  // namespace lb::proxy
