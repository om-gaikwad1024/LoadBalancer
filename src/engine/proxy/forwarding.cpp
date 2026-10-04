#include "proxy/forwarding.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

namespace lb::proxy {

namespace {

constexpr std::size_t kMaxRequestIdLength = 128;

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parse_ipv4(std::string_view text, std::uint32_t* out) {
    if (text.empty() || text.size() > 15) return false;
    const std::string s(text);
    in_addr a{};
    if (::inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    *out = ::ntohl(a.s_addr);
    return true;
}

}  // namespace

std::string format_ipv4(std::uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    return buf;
}

bool valid_request_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxRequestIdLength) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

std::string choose_request_id(const http::Fields& fields, bool peer_trusted, std::string_view generated) {
    if (peer_trusted && fields.count("X-Request-Id") == 1) {
        const std::string& upstream = *fields.find("X-Request-Id");
        if (valid_request_id(upstream)) return upstream;
    }
    return std::string(generated);
}

std::string client_identity(std::uint32_t peer, const http::Fields& fields, const ConfigSnapshot& config) {
    if (!config.is_trusted_proxy(peer)) return format_ipv4(peer);

    // Every X-Forwarded-For value, in order, as one list.
    std::vector<std::string_view> hops;
    for (const auto& f : fields.all()) {
        if (!http::iequals(f.name, "X-Forwarded-For")) continue;
        std::string_view v = f.value;
        while (true) {
            const auto comma = v.find(',');
            hops.push_back(trim(v.substr(0, comma)));
            if (comma == std::string_view::npos) break;
            v.remove_prefix(comma + 1);
        }
    }

    std::uint32_t identity = peer;
    for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
        std::uint32_t hop = 0;
        if (!parse_ipv4(*it, &hop)) break;  // malformed: trust nothing further left
        identity = hop;
        if (!config.is_trusted_proxy(hop)) break;  // first untrusted hop is the client
    }
    return format_ipv4(identity);
}

RequestIdGenerator::RequestIdGenerator() {
    std::random_device rd;
    prefix_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
}

std::string RequestIdGenerator::make(std::uint64_t sequence) const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(prefix_),
                  static_cast<unsigned long long>(sequence));
    return buf;
}

}  // namespace lb::proxy
