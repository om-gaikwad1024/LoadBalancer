#include <gtest/gtest.h>

#include <string>

#include "http/http_parser.h"
#include "http_test_util.h"

using lb::http::BodyFraming;
using lb::http::HttpParser;
using lb::http::ParseEvent;
using lb::http::Version;
using lbtest::parse_request;
using lbtest::parse_response;

namespace {

std::string printable(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) out += "\\x" + std::to_string(static_cast<unsigned char>(c));
        else out += c;
    }
    return out;
}

std::string with_body(std::string head, std::string_view body) { return head + std::string(body); }

}  // namespace

#define EXPECT_REQUEST_REJECTED(input, status)                                          \
    do {                                                                                \
        const auto o_ = parse_request(input);                                           \
        EXPECT_EQ(o_.last, ParseEvent::Error) << "accepted: " << printable(input);      \
        EXPECT_EQ(o_.error_status, (status)) << o_.error_detail << " | " << printable(input); \
    } while (0)

#define EXPECT_RESPONSE_REJECTED(input)                                                 \
    do {                                                                                \
        const auto o_ = parse_response(input);                                          \
        EXPECT_EQ(o_.last, ParseEvent::Error) << "accepted: " << printable(input);      \
        EXPECT_EQ(o_.error_status, 502) << o_.error_detail << " | " << printable(input); \
    } while (0)

// ---- Requests: accepted ---------------------------------------------------------------

TEST(HttpRequestParser, SimpleGet) {
    const std::string in = "GET /index.html?q=1 HTTP/1.1\r\nHost: example.com\r\nUser-Agent: t\r\n\r\n";
    const auto o = parse_request(in);
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_TRUE(o.head_seen);
    EXPECT_EQ(o.request.method, "GET");
    EXPECT_EQ(o.request.target, "/index.html?q=1");
    EXPECT_EQ(o.request.version, Version::Http11);
    EXPECT_EQ(o.request.fields.size(), 2u);
    EXPECT_EQ(*o.request.fields.find("host"), "example.com");
    EXPECT_EQ(o.request.framing, BodyFraming::None);
    EXPECT_TRUE(o.request.keep_alive);
    EXPECT_TRUE(o.body.empty());
    EXPECT_EQ(o.consumed, in.size());
}

TEST(HttpRequestParser, Http10DefaultsToCloseAndNeedsNoHost) {
    auto o = parse_request("GET / HTTP/1.0\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.request.version, Version::Http10);
    EXPECT_FALSE(o.request.keep_alive);

    o = parse_request("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete);
    EXPECT_TRUE(o.request.keep_alive);
}

TEST(HttpRequestParser, ConnectionCloseTokenDisablesKeepAlive) {
    const auto o = parse_request("GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive, Close\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete);
    EXPECT_FALSE(o.request.keep_alive);
}

TEST(HttpRequestParser, ContentLengthBody) {
    const auto o = parse_request("POST /f HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhello world");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.request.framing, BodyFraming::ContentLength);
    EXPECT_EQ(o.request.content_length, 11u);
    EXPECT_EQ(o.body, "hello world");
}

TEST(HttpRequestParser, ContentLengthZeroCompletesWithHead) {
    const std::string in = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n";
    const auto o = parse_request(in);
    ASSERT_EQ(o.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o.body_events, 0);
    EXPECT_EQ(o.consumed, in.size());
}

TEST(HttpRequestParser, ChunkedBodyIsDecodedAndTrailersDropped) {
    const std::string in =
        "POST /u HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n"
        "6;name=\"v\"\r\n world\r\n"
        "000A ; x\r\n0123456789\r\n"
        "0\r\nX-Checksum: abc\r\n\r\n";
    const auto o = parse_request(in);
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.request.framing, BodyFraming::Chunked);
    EXPECT_EQ(o.body, "hello world0123456789");
    EXPECT_EQ(o.consumed, in.size());
}

TEST(HttpRequestParser, ChunkSizeAcceptsUpperAndLowerHex) {
    const std::string data(0x1F, 'z');
    const auto o = parse_request("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n1F\r\n" + data +
                                 "\r\n1f\r\n" + data + "\r\n0\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.body, data + data);
}

TEST(HttpRequestParser, FieldValuesAreTrimmedAndMayBeEmptyOrObsText) {
    const auto o = parse_request("GET / HTTP/1.1\r\nHost: a\r\nX-A: \t spaced \t\r\nX-Empty:\r\nX-Obs: caf\xC3\xA9\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(*o.request.fields.find("x-a"), "spaced");
    EXPECT_EQ(*o.request.fields.find("X-EMPTY"), "");
    EXPECT_EQ(*o.request.fields.find("x-obs"), "caf\xC3\xA9");
}

TEST(HttpRequestParser, PipelinedRequestsStopAtMessageBoundary) {
    const std::string first = "POST /1 HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\nabc";
    const std::string second = "GET /2 HTTP/1.1\r\nHost: a\r\n\r\n";
    const std::string both = first + second;

    HttpParser p(HttpParser::Kind::Request, lbtest::small_request_limits());
    const auto o1 = lbtest::run(p, both, both.size(), 0, false);
    ASSERT_EQ(o1.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o1.consumed, first.size());
    EXPECT_EQ(o1.body, "abc");

    p.reset();
    const auto o2 = lbtest::run(p, std::string_view(both).substr(o1.consumed), both.size(), 0, false);
    ASSERT_EQ(o2.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o2.request.target, "/2");
}

TEST(HttpRequestParser, EmptyLinesBeforeRequestLineAreIgnored) {
    const auto o = parse_request("\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.request.method, "GET");
}

TEST(HttpRequestParser, TargetForms) {
    EXPECT_EQ(parse_request("GET http://example.com:8080/p?q HTTP/1.1\r\nHost: example.com\r\n\r\n").last,
              ParseEvent::MessageComplete);
    EXPECT_EQ(parse_request("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n").last, ParseEvent::MessageComplete);
    EXPECT_REQUEST_REJECTED("GET * HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET relative/path HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET http:// HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET http:///x HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET ftp://a/x HTTP/1.1\r\nHost: a\r\n\r\n", 400);
}

TEST(HttpRequestParser, HigherMinorVersionIsTreatedAsHttp11) {
    const auto o = parse_request("GET / HTTP/1.7\r\nHost: a\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o.request.version, Version::Http11);
}

TEST(HttpRequestParser, HugeButValidContentLengthIsStreamed) {
    const auto o = parse_request("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 18446744073709551615\r\n\r\nabc");
    EXPECT_EQ(o.last, ParseEvent::NeedMore);
    EXPECT_TRUE(o.head_seen);
    EXPECT_EQ(o.request.content_length, 18446744073709551615ull);
    EXPECT_EQ(o.body, "abc");
}

TEST(HttpRequestParser, FieldLookupsAreCaseInsensitive) {
    const auto o = parse_request("GET / HTTP/1.1\r\nHost: a\r\nX-List: a, B ,c\r\nx-list: d\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o.request.fields.count("X-LIST"), 2u);
    EXPECT_TRUE(o.request.fields.has_token("x-list", "b"));
    EXPECT_TRUE(o.request.fields.has_token("x-list", "D"));
    EXPECT_FALSE(o.request.fields.has_token("x-list", "e"));
    EXPECT_EQ(o.request.fields.find("missing"), nullptr);
}

// ---- Requests: limits (431 or 400, plan IV.3) -----------------------------------------

TEST(HttpRequestParser, RequestLineLimitIsExact) {
    const auto line = [](std::size_t len) { return "GET /" + std::string(len - 14, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n"; };
    EXPECT_EQ(parse_request(line(100)).last, ParseEvent::MessageComplete);
    EXPECT_REQUEST_REJECTED(line(101), 400);
}

TEST(HttpRequestParser, HeaderSectionByteLimitIsExact) {
    // "Host: x\r\n" (9) + "X: " + value + "\r\n" (5 + n) = 14 + n bytes of fields.
    const auto req = [](std::size_t n) { return "GET / HTTP/1.1\r\nHost: x\r\nX: " + std::string(n, 'v') + "\r\n\r\n"; };
    EXPECT_EQ(parse_request(req(286)).last, ParseEvent::MessageComplete);
    EXPECT_REQUEST_REJECTED(req(287), 431);
    EXPECT_REQUEST_REJECTED(req(5000), 431);  // rejected early, before the line ends
}

TEST(HttpRequestParser, HeaderCountLimitIsExact) {
    const auto req = [](int n) {
        std::string s = "GET / HTTP/1.1\r\nHost: a\r\n";
        for (int i = 1; i < n; ++i) s += "X" + std::to_string(i) + ": v\r\n";
        return s + "\r\n";
    };
    EXPECT_EQ(parse_request(req(10)).last, ParseEvent::MessageComplete);
    EXPECT_REQUEST_REJECTED(req(11), 431);
}

TEST(HttpRequestParser, ChunkLineLimitIsExact) {
    const auto req = [](std::size_t ext) {
        return "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n1;" + std::string(ext, 'e') +
               "\r\nx\r\n0\r\n\r\n";
    };
    EXPECT_EQ(parse_request(req(30)).last, ParseEvent::MessageComplete);  // 32-byte line
    EXPECT_REQUEST_REJECTED(req(31), 400);
}

TEST(HttpRequestParser, TrailerLimits) {
    std::string many = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
    for (int i = 0; i < 11; ++i) many += "T" + std::to_string(i) + ": v\r\n";
    EXPECT_REQUEST_REJECTED(many + "\r\n", 431);
    EXPECT_REQUEST_REJECTED(
        "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nT: " + std::string(400, 'v') + "\r\n\r\n",
        431);
}

// ---- Requests: framing and smuggling defense (plan VII) -------------------------------

TEST(HttpRequestParser, ContentLengthAndTransferEncodingTogetherAreRejected) {
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 6\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nG", 400);
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n0\r\n\r\n", 400);
}

TEST(HttpRequestParser, DuplicateOrConflictingContentLengthIsRejected) {
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello", 400);
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 400);
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\n\r\nhello", 400);
}

TEST(HttpRequestParser, MalformedContentLengthIsRejected) {
    for (const char* value : {"+5", "-5", "5 5", "0x5", "", "5a", "1e3", "18446744073709551616", "99999999999999999999"}) {
        SCOPED_TRACE(value);
        EXPECT_REQUEST_REJECTED(std::string("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: ") + value + "\r\n\r\n", 400);
    }
}

TEST(HttpRequestParser, OnlyASingleChunkedCodingIsAccepted) {
    const auto te = [](const std::string& value) {
        return "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: " + value + "\r\n\r\n0\r\n\r\n";
    };
    EXPECT_REQUEST_REJECTED(te("gzip"), 501);
    EXPECT_REQUEST_REJECTED(te("gzip, chunked"), 501);
    EXPECT_REQUEST_REJECTED(te("xchunked"), 501);
    EXPECT_REQUEST_REJECTED(te("chunked;q=1"), 501);
    EXPECT_REQUEST_REJECTED(te("chunked, chunked"), 400);
    EXPECT_REQUEST_REJECTED(te("chunked,"), 400);
    EXPECT_REQUEST_REJECTED(te(""), 400);
    EXPECT_REQUEST_REJECTED(te("chunked\x0b"), 400);
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding : chunked\r\n\r\n0\r\n\r\n", 400);
}

TEST(HttpRequestParser, MalformedChunksAreRejected) {
    const std::string head = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n";
    EXPECT_REQUEST_REJECTED(head + "zz\r\n", 400);                    // not hex
    EXPECT_REQUEST_REJECTED(head + "\r\n", 400);                      // empty size
    EXPECT_REQUEST_REJECTED(head + "-1\r\n", 400);                    // sign
    EXPECT_REQUEST_REJECTED(head + "0x5\r\nhello\r\n0\r\n\r\n", 400);  // 0x prefix
    EXPECT_REQUEST_REJECTED(head + "10000000000000000\r\n", 400);     // 2^64: overflow
    EXPECT_REQUEST_REJECTED(head + "5\r\nhelloXX0\r\n\r\n", 400);     // data not followed by CRLF
    EXPECT_REQUEST_REJECTED(head + "5\nhello\r\n0\r\n\r\n", 400);     // bare LF
    EXPECT_REQUEST_REJECTED(head + "5\rhello\r\n0\r\n\r\n", 400);     // bare CR
    EXPECT_REQUEST_REJECTED(head + "5 \r\nhello\r\n0\r\n\r\n", 400);  // whitespace without extension
    EXPECT_REQUEST_REJECTED(head + "5;e\x01\r\nhello\r\n0\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED(head + "0\r\nbad trailer\r\n\r\n", 400);
}

TEST(HttpRequestParser, MalformedHeadsAreRejected) {
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\nHost: a\n\n", 400);                    // bare LF
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\rX: b\r\n\r\n", 400);        // bare CR
    static constexpr char kNul[] = "GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n";
    EXPECT_REQUEST_REJECTED(std::string(kNul, sizeof(kNul) - 1), 400);  // NUL
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\r\nX: a\r\n  folded\r\n\r\n", 400);  // obs-fold
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\r\n: novalue\r\n\r\n", 400);  // empty name
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\r\nBad[Name]: x\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\r\nX: a\x7F\r\n\r\n", 400);
}

TEST(HttpRequestParser, HostHeaderRules) {
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a b\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1\r\nHost: a/b\r\n\r\n", 400);
    EXPECT_EQ(parse_request("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n").last, ParseEvent::MessageComplete);
}

TEST(HttpRequestParser, MalformedRequestLinesAreRejected) {
    EXPECT_REQUEST_REJECTED("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED(" GET / HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET /\tHTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / http/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET / HTTP/1.10\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET /\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("G(T / HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET /a#frag HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("GET /caf\xC3\xA9 HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_REQUEST_REJECTED("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 400);  // TLS hello on a plain port
}

TEST(HttpRequestParser, UnsupportedVersionsAndConnect) {
    EXPECT_REQUEST_REJECTED("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505);
    EXPECT_REQUEST_REJECTED("GET / HTTP/0.9\r\nHost: a\r\n\r\n", 505);
    EXPECT_REQUEST_REJECTED("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n", 501);
}

// ---- Parser lifecycle ----------------------------------------------------------------

TEST(HttpParserLifecycle, IdleAndCleanClose) {
    HttpParser p(HttpParser::Kind::Request, lbtest::small_request_limits());
    EXPECT_TRUE(p.idle());
    EXPECT_EQ(p.finish().event, ParseEvent::NeedMore);  // nothing in progress: clean close
    EXPECT_EQ(p.parse("GE").event, ParseEvent::NeedMore);
    EXPECT_FALSE(p.idle());
    EXPECT_EQ(p.finish().event, ParseEvent::Error);  // closed mid-message
}

TEST(HttpParserLifecycle, TruncatedBodyIsAnErrorOnClose) {
    const auto o = parse_request("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc", lbtest::small_request_limits(), true);
    EXPECT_EQ(o.last, ParseEvent::Error);
    EXPECT_EQ(o.body, "abc");
}

TEST(HttpParserLifecycle, ErrorIsSticky) {
    HttpParser p(HttpParser::Kind::Request, lbtest::small_request_limits());
    EXPECT_EQ(p.parse("GET / HTTP/1.1\n").event, ParseEvent::Error);
    EXPECT_EQ(p.parse("Host: a\r\n\r\n").event, ParseEvent::Error);
    EXPECT_EQ(p.error().status, 400);
}

TEST(HttpParserLifecycle, ResetAllowsTheNextMessage) {
    HttpParser p(HttpParser::Kind::Request, lbtest::small_request_limits());
    for (int i = 0; i < 3; ++i) {
        const std::string in = "GET /" + std::to_string(i) + " HTTP/1.1\r\nHost: a\r\n\r\n";
        const auto o = lbtest::run(p, in, in.size(), 0, false);
        ASSERT_EQ(o.last, ParseEvent::MessageComplete);
        EXPECT_EQ(o.request.target, "/" + std::to_string(i));
        p.reset();
        EXPECT_TRUE(p.idle());
    }
}

// ---- Responses (framing decides pool reuse, plan IV.3/IV.5) ---------------------------

TEST(HttpResponseParser, ContentLengthResponseIsReusable) {
    const std::string in = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    const auto o = parse_response(in);
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.response.status, 200);
    EXPECT_EQ(o.response.reason, "OK");
    EXPECT_EQ(o.body, "hello");
    EXPECT_TRUE(o.response.keep_alive);
    EXPECT_EQ(o.consumed, in.size());
}

TEST(HttpResponseParser, ChunkedResponseIsDecoded) {
    const auto o = parse_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.response.framing, BodyFraming::Chunked);
    EXPECT_EQ(o.body, "abcde");
    EXPECT_TRUE(o.response.keep_alive);
}

TEST(HttpResponseParser, CloseDelimitedBodyEndsAtCloseAndIsNotReusable) {
    const auto o = parse_response("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the end", false, true);
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.response.framing, BodyFraming::UntilClose);
    EXPECT_EQ(o.body, "until the end");
    EXPECT_FALSE(o.response.keep_alive);
}

TEST(HttpResponseParser, ResponsesWithoutBody) {
    const std::string head_resp = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
    auto o = parse_response(head_resp, /*request_was_head=*/true);
    ASSERT_EQ(o.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o.consumed, head_resp.size());
    EXPECT_TRUE(o.response.keep_alive);

    for (const char* status : {"204 No Content", "304 Not Modified"}) {
        o = parse_response(std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 10\r\n\r\n");
        ASSERT_EQ(o.last, ParseEvent::MessageComplete) << status;
        EXPECT_TRUE(o.body.empty());
    }
}

TEST(HttpResponseParser, InterimResponseThenFinal) {
    const std::string interim = "HTTP/1.1 100 Continue\r\n\r\n";
    const std::string final_resp = "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
    const std::string both = interim + final_resp;

    HttpParser p(HttpParser::Kind::Response, lbtest::small_response_limits());
    p.reset_for_response(false);
    const auto o1 = lbtest::run(p, both, both.size(), 0, false);
    ASSERT_EQ(o1.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o1.response.status, 100);
    EXPECT_EQ(o1.consumed, interim.size());

    p.reset_for_response(false);
    const auto o2 = lbtest::run(p, std::string_view(both).substr(o1.consumed), both.size(), 0, false);
    ASSERT_EQ(o2.last, ParseEvent::MessageComplete);
    EXPECT_EQ(o2.response.status, 201);
    EXPECT_EQ(o2.body, "ok");
}

TEST(HttpResponseParser, KeepAliveRules) {
    EXPECT_FALSE(parse_response("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n").response.keep_alive);
    EXPECT_FALSE(parse_response("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n").response.keep_alive);
    EXPECT_TRUE(parse_response("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n").response.keep_alive);
}

TEST(HttpResponseParser, MissingReasonPhraseIsAccepted) {
    const auto o = parse_response("HTTP/1.1 404\r\nContent-Length: 0\r\n\r\n");
    ASSERT_EQ(o.last, ParseEvent::MessageComplete) << o.error_detail;
    EXPECT_EQ(o.response.status, 404);
    EXPECT_EQ(o.response.reason, "");
}

TEST(HttpResponseParser, BadResponsesAre502) {
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 2000 OK\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 099 Low\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200OK\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/2 200 OK\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/2.0 200 OK\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("\r\nHTTP/1.1 200 OK\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nX: a\r\n folded\r\nContent-Length: 0\r\n\r\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\nContent-Length: 0\n\n");
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
}

TEST(HttpResponseParser, HeadLimitAndTruncation) {
    EXPECT_RESPONSE_REJECTED("HTTP/1.1 200 OK\r\nX: " + std::string(500, 'v') + "\r\n\r\n");
    const auto o = parse_response("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", false, true);
    EXPECT_EQ(o.last, ParseEvent::Error);
    EXPECT_EQ(o.error_status, 502);
}
