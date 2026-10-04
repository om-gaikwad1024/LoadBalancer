#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "http/http_parser.h"

namespace lbtest {

using lb::http::HttpParser;
using lb::http::ParseEvent;
using lb::http::ParserLimits;

// Small limits so boundary tests stay readable.
inline ParserLimits small_request_limits() {
    ParserLimits l;
    l.max_start_line_bytes = 100;
    l.max_field_section_bytes = 300;
    l.max_head_bytes = SIZE_MAX;
    l.max_field_count = 10;
    l.max_chunk_line_bytes = 32;
    return l;
}

inline ParserLimits small_response_limits() {
    ParserLimits l = small_request_limits();
    l.max_head_bytes = 400;
    return l;
}

struct Outcome {
    ParseEvent last = ParseEvent::NeedMore;  // MessageComplete, Error, or NeedMore (input ran out)
    int error_status = 0;
    std::string error_detail;
    bool head_seen = false;
    lb::http::RequestHead request;
    lb::http::ResponseHead response;
    std::string body;
    std::size_t consumed = 0;
    int body_events = 0;
};

inline bool same_result(const Outcome& a, const Outcome& b) {
    return a.last == b.last && a.error_status == b.error_status && a.head_seen == b.head_seen && a.body == b.body &&
           a.consumed == b.consumed && a.request.method == b.request.method && a.request.target == b.request.target &&
           a.request.fields.size() == b.request.fields.size() && a.response.status == b.response.status &&
           a.response.fields.size() == b.response.fields.size();
}

// Feeds `input` in pieces until the message completes, fails, or input runs out.
// piece == 0 means random piece sizes (1..17) from `seed`. With finish_at_end the peer
// "closes" after the last byte (needed for close-delimited responses and truncation).
inline Outcome run(HttpParser& parser, std::string_view input, std::size_t piece, std::uint32_t seed,
                   bool finish_at_end) {
    Outcome o;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> random_piece(1, 17);
    std::size_t offset = 0;
    std::string_view pending;
    // After HeadComplete or Body the parser may have more to report without new input
    // (e.g. MessageComplete), so it is called again even when nothing is pending.
    bool poll = false;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        if (pending.empty() && !poll) {
            if (offset == input.size()) {
                if (finish_at_end) {
                    const auto r = parser.finish();
                    o.last = r.event;
                    if (r.event == ParseEvent::Error) {
                        o.error_status = parser.error().status;
                        o.error_detail = std::string(parser.error().detail);
                    }
                }
                return o;
            }
            const std::size_t n = std::min(piece == 0 ? random_piece(rng) : piece, input.size() - offset);
            pending = input.substr(offset, n);
            offset += n;
        }
        const auto r = parser.parse(pending);
        EXPECT_LE(r.consumed, pending.size());
        o.consumed += r.consumed;
        pending.remove_prefix(std::min(r.consumed, pending.size()));
        poll = false;
        switch (r.event) {
            case ParseEvent::NeedMore:
                EXPECT_TRUE(pending.empty()) << "NeedMore must consume all input";
                pending = {};
                break;
            case ParseEvent::HeadComplete:
                o.head_seen = true;
                o.request = parser.request();
                o.response = parser.response();
                poll = true;
                break;
            case ParseEvent::Body:
                EXPECT_FALSE(r.body.empty());
                o.body += r.body;
                ++o.body_events;
                poll = true;
                break;
            case ParseEvent::MessageComplete:
                o.last = r.event;
                return o;
            case ParseEvent::Error:
                o.last = r.event;
                o.error_status = parser.error().status;
                o.error_detail = std::string(parser.error().detail);
                EXPECT_FALSE(o.error_detail.empty());
                return o;
        }
    }
    ADD_FAILURE() << "parser made no progress";
    return o;
}

// Parses `input` whole, then byte-by-byte, in fixed pieces and in random pieces; every
// split must give the same result (plan IV.3: messages split across many TCP reads).
template <typename MakeParser>
Outcome parse_all_splits(std::string_view input, MakeParser make, bool finish_at_end) {
    auto whole_parser = make();
    const Outcome whole = run(whole_parser, input, input.size() + 1, 0, finish_at_end);
    const std::size_t fixed[] = {1, 2, 3, 5, 7, 13};
    for (std::size_t piece : fixed) {
        auto p = make();
        const Outcome o = run(p, input, piece, 0, finish_at_end);
        EXPECT_TRUE(same_result(whole, o)) << "split into pieces of " << piece << " differs from whole input";
    }
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        auto p = make();
        const Outcome o = run(p, input, 0, seed, finish_at_end);
        EXPECT_TRUE(same_result(whole, o)) << "random split seed " << seed << " differs from whole input";
    }
    return whole;
}

inline Outcome parse_request(std::string_view input, const ParserLimits& limits = small_request_limits(),
                             bool finish_at_end = false) {
    return parse_all_splits(input, [&] { return HttpParser(HttpParser::Kind::Request, limits); }, finish_at_end);
}

inline Outcome parse_response(std::string_view input, bool request_was_head = false, bool finish_at_end = false,
                              const ParserLimits& limits = small_response_limits()) {
    return parse_all_splits(
        input,
        [&] {
            HttpParser p(HttpParser::Kind::Response, limits);
            p.reset_for_response(request_was_head);
            return p;
        },
        finish_at_end);
}

// Corpus files are written with C-style escapes so exact bytes survive editors and git:
// \r \n \t \\ \xHH are decoded; literal line breaks in the file are ignored.
inline std::string decode_escaped(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r' || c == '\n') continue;
        if (c != '\\' || i + 1 >= text.size()) {
            out += c;
            continue;
        }
        const char e = text[++i];
        switch (e) {
            case 'r': out += '\r'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '\\': out += '\\'; break;
            case 'x': {
                if (i + 2 >= text.size()) {
                    ADD_FAILURE() << "truncated \\x escape";
                    return out;
                }
                out +=static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
                i += 2;
                break;
            }
            default: out += '\\'; out += e; break;
        }
    }
    return out;
}

}  // namespace lbtest
