// libFuzzer target for the HTTP parser (plan IX "Fuzzing"): the parser must never
// crash, hang, or read out of bounds on arbitrary bytes. Built only by the `fuzz`
// preset with /fsanitize=address and /fsanitize=fuzzer.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "http/http_parser.h"

namespace {

lb::http::ParserLimits fuzz_limits() {
    lb::http::ParserLimits l;
    l.max_start_line_bytes = 1024;
    l.max_field_section_bytes = 2048;
    l.max_head_bytes = 4096;
    l.max_field_count = 32;
    l.max_chunk_line_bytes = 64;
    return l;
}

void check(bool condition) {
    if (!condition) std::abort();
}

// Feeds `input` in pieces of `piece` bytes, across as many messages as it contains.
void drive(lb::http::HttpParser& parser, std::string_view input, std::size_t piece, bool response) {
    using lb::http::ParseEvent;
    std::size_t offset = 0;
    std::string_view pending;
    int stalls = 0;
    while (true) {
        if (pending.empty()) {
            if (offset == input.size()) {
                parser.finish();
                return;
            }
            const std::size_t n = piece < input.size() - offset ? piece : input.size() - offset;
            pending = input.substr(offset, n);
            offset += n;
        }
        const auto r = parser.parse(pending);
        check(r.consumed <= pending.size());
        pending.remove_prefix(r.consumed);
        switch (r.event) {
            case ParseEvent::NeedMore:
                check(pending.empty());
                break;
            case ParseEvent::HeadComplete:
            case ParseEvent::Body:
                check(r.consumed > 0 || r.event == ParseEvent::HeadComplete);
                break;
            case ParseEvent::MessageComplete:
                if (response) parser.reset_for_response(false);
                else parser.reset();
                break;
            case ParseEvent::Error:
                check(parser.error().status >= 400 && !parser.error().detail.empty());
                return;
        }
        // Each call must make progress unless a message just ended.
        stalls = r.consumed == 0 ? stalls + 1 : 0;
        check(stalls < 4);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    // First byte selects the read size, so split reads are fuzzed too.
    const std::size_t piece = static_cast<std::size_t>(data[0] % 16) + 1;
    const std::string_view input(reinterpret_cast<const char*>(data + 1), size - 1);

    lb::http::HttpParser request(lb::http::HttpParser::Kind::Request, fuzz_limits());
    drive(request, input, piece, false);

    lb::http::HttpParser response(lb::http::HttpParser::Kind::Response, fuzz_limits());
    response.reset_for_response((data[0] & 0x80) != 0);
    drive(response, input, piece, true);
    return 0;
}
