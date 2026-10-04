#include <gtest/gtest.h>

#include <set>
#include <string>

#include "http/http_parser.h"
#include "proxy/forwarding.h"
#include "proxy/http_writer.h"

using lb::proxy::choose_request_id;
using lb::proxy::client_identity;
using lb::proxy::ForwardingContext;

namespace {

lb::http::RequestHead parse_head(const std::string& text) {
    lb::http::ParserLimits limits;
    limits.max_start_line_bytes = 1024;
    limits.max_field_section_bytes = 4096;
    limits.max_head_bytes = SIZE_MAX;
    limits.max_field_count = 50;
    limits.max_chunk_line_bytes = 64;
    lb::http::HttpParser p(lb::http::HttpParser::Kind::Request, limits);
    const auto r = p.parse(text);
    EXPECT_EQ(r.event, lb::http::ParseEvent::HeadComplete) << p.error().detail;
    return p.request();
}

lb::http::Fields xff(std::initializer_list<const char*> values) {
    lb::http::Fields f;
    for (const char* v : values) f.add("X-Forwarded-For", v);
    return f;
}

lb::ConfigSnapshot trusting(std::initializer_list<lb::Ipv4Cidr> cidrs) {
    lb::ConfigSnapshot c;
    c.trusted_proxies = cidrs;
    return c;
}

constexpr std::uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }

const lb::Ipv4Cidr kTenSlash8{ip(10, 0, 0, 0), 0xFF000000};
const lb::Ipv4Cidr kLoopback{ip(127, 0, 0, 1), 0xFFFFFFFF};

std::string backend_head(const std::string& request, const ForwardingContext& fwd) {
    std::string out;
    lb::proxy::append_backend_request_head(out, parse_head(request), fwd);
    return out;
}

ForwardingContext context(bool trusted = false) {
    ForwardingContext f;
    f.client_ip = "192.0.2.10";
    f.peer_trusted = trusted;
    f.proto = "http";
    f.request_id = "abc123";
    f.backend_endpoint = "10.1.1.1:8080";
    return f;
}

}  // namespace

// ---- Trust rule: rate-limit identity (plan IV.6, VII) ---------------------------------

TEST(ClientIdentity, UntrustedPeerIsItselfWhateverXffSays) {
    const auto config = trusting({});
    EXPECT_EQ(client_identity(ip(192, 0, 2, 10), xff({"6.6.6.6"}), config), "192.0.2.10");
    EXPECT_EQ(client_identity(ip(192, 0, 2, 10), {}, config), "192.0.2.10");
}

TEST(ClientIdentity, SpoofedXffDoesNotChangeTheIdentity) {
    const auto config = trusting({kTenSlash8});
    const std::uint32_t peer = ip(203, 0, 113, 5);  // not a trusted proxy
    EXPECT_EQ(client_identity(peer, xff({"1.1.1.1"}), config), client_identity(peer, xff({"2.2.2.2"}), config));
    EXPECT_EQ(client_identity(peer, xff({"1.1.1.1"}), config), "203.0.113.5");
}

TEST(ClientIdentity, TrustedPeerYieldsTheFirstUntrustedHopFromTheRight) {
    const auto config = trusting({kTenSlash8});
    const std::uint32_t lb_peer = ip(10, 0, 0, 2);
    EXPECT_EQ(client_identity(lb_peer, xff({"198.51.100.7"}), config), "198.51.100.7");
    // client spoofed 6.6.6.6, a trusted hop appended the real client 198.51.100.7, then 10.0.0.3
    EXPECT_EQ(client_identity(lb_peer, xff({"6.6.6.6, 198.51.100.7", "10.0.0.3"}), config), "198.51.100.7");
}

TEST(ClientIdentity, AllTrustedHopsMeansTheLeftmost) {
    const auto config = trusting({kTenSlash8});
    EXPECT_EQ(client_identity(ip(10, 0, 0, 2), xff({"10.9.9.9, 10.0.0.3"}), config), "10.9.9.9");
}

TEST(ClientIdentity, MalformedHopStopsTheWalk) {
    const auto config = trusting({kTenSlash8});
    EXPECT_EQ(client_identity(ip(10, 0, 0, 2), xff({"198.51.100.7, garbage, 10.0.0.3"}), config), "10.0.0.3");
    EXPECT_EQ(client_identity(ip(10, 0, 0, 2), xff({"not-an-ip"}), config), "10.0.0.2");
    EXPECT_EQ(client_identity(ip(10, 0, 0, 2), {}, config), "10.0.0.2");
}

// ---- Request ids -------------------------------------------------------------------------

TEST(RequestId, GeneratedIdsAre32HexCharactersAndUnique) {
    lb::proxy::RequestIdGenerator gen;
    std::set<std::string> seen;
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        const auto id = gen.make(i);
        ASSERT_EQ(id.size(), 32u);
        ASSERT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos) << id;
        seen.insert(id);
    }
    EXPECT_EQ(seen.size(), 1000u);
}

TEST(RequestId, OnlyAWellFormedIdFromATrustedPeerIsKept) {
    lb::http::Fields upstream;
    upstream.add("X-Request-Id", "edge-42.a_b");
    EXPECT_EQ(choose_request_id(upstream, /*trusted=*/true, "gen"), "edge-42.a_b");
    EXPECT_EQ(choose_request_id(upstream, /*trusted=*/false, "gen"), "gen");

    lb::http::Fields bad;
    bad.add("X-Request-Id", "has space");
    EXPECT_EQ(choose_request_id(bad, true, "gen"), "gen");
    lb::http::Fields two;
    two.add("X-Request-Id", "a");
    two.add("X-Request-Id", "b");
    EXPECT_EQ(choose_request_id(two, true, "gen"), "gen");
    EXPECT_FALSE(lb::proxy::valid_request_id(std::string(129, 'a')));
    EXPECT_FALSE(lb::proxy::valid_request_id(""));
}

// ---- Backend request head (plan IV.6) --------------------------------------------------

TEST(ForwardingHeaders, AddedWhenAbsent) {
    const auto head = backend_head("GET /p HTTP/1.1\r\nHost: shop.example\r\n\r\n", context());
    EXPECT_NE(head.find("\r\nHost: shop.example\r\n"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Forwarded-For: 192.0.2.10\r\n"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Forwarded-Proto: http\r\n"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Forwarded-Host: shop.example\r\n"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Request-Id: abc123\r\n"), std::string::npos) << head;
}

TEST(ForwardingHeaders, XffIsAppendedToEveryExistingValueInOrder) {
    const auto head = backend_head(
        "GET / HTTP/1.1\r\nHost: h\r\nX-Forwarded-For: 1.1.1.1\r\nX-Forwarded-For: 2.2.2.2, 3.3.3.3\r\n\r\n", context());
    EXPECT_NE(head.find("\r\nX-Forwarded-For: 1.1.1.1, 2.2.2.2, 3.3.3.3, 192.0.2.10\r\n"), std::string::npos) << head;
    EXPECT_EQ(head.find("X-Forwarded-For", head.find("X-Forwarded-For") + 1), std::string::npos) << "one merged field";
}

TEST(ForwardingHeaders, UntrustedClientValuesAreReplaced) {
    const auto head = backend_head(
        "GET / HTTP/1.1\r\nHost: real.example\r\nX-Forwarded-Proto: https\r\nX-Forwarded-Host: evil.example\r\n"
        "X-Request-Id: spoofed\r\n\r\n",
        context(/*trusted=*/false));
    EXPECT_EQ(head.find("https"), std::string::npos) << head;
    EXPECT_EQ(head.find("evil.example"), std::string::npos) << head;
    EXPECT_EQ(head.find("spoofed"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Forwarded-Host: real.example\r\n"), std::string::npos) << head;
}

TEST(ForwardingHeaders, TrustedProxyValuesAreKept) {
    const auto head = backend_head(
        "GET / HTTP/1.1\r\nHost: internal\r\nX-Forwarded-Proto: https\r\nX-Forwarded-Host: www.example\r\n\r\n",
        context(/*trusted=*/true));
    EXPECT_NE(head.find("\r\nX-Forwarded-Proto: https\r\n"), std::string::npos) << head;
    EXPECT_EQ(head.find("X-Forwarded-Proto: http\r\n"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Forwarded-Host: www.example\r\n"), std::string::npos) << head;
    EXPECT_EQ(head.find("X-Forwarded-Host: internal"), std::string::npos) << head;
}

TEST(ForwardingHeaders, HostRewriteModeUsesTheBackendEndpoint) {
    auto fwd = context();
    fwd.host_mode = lb::HostHeaderMode::Backend;
    const auto head = backend_head("GET / HTTP/1.1\r\nHost: shop.example\r\n\r\n", fwd);
    EXPECT_NE(head.find("\r\nHost: 10.1.1.1:8080\r\n"), std::string::npos) << head;
    EXPECT_NE(head.find("\r\nX-Forwarded-Host: shop.example\r\n"), std::string::npos) << head;
}

TEST(ForwardingHeaders, Http10WithoutHostGetsTheBackendHost) {
    const auto head = backend_head("GET / HTTP/1.0\r\n\r\n", context());
    EXPECT_NE(head.find("\r\nHost: 10.1.1.1:8080\r\n"), std::string::npos) << head;
    EXPECT_EQ(head.find("X-Forwarded-Host"), std::string::npos) << head;
}
