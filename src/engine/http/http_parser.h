#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "config/config.h"
#include "http/http_message.h"

namespace lb::http {

struct ParserLimits {
    std::size_t max_start_line_bytes = 0;     // request/status line, excluding CRLF
    std::size_t max_field_section_bytes = 0;  // header (and trailer) field lines including their CRLFs
    std::size_t max_head_bytes = 0;           // whole head including the final empty line
    std::size_t max_field_count = 0;
    std::size_t max_chunk_line_bytes = 0;     // chunk-size line excluding CRLF
};

ParserLimits request_parser_limits(const LimitsConfig& limits) noexcept;
ParserLimits response_parser_limits(const LimitsConfig& limits) noexcept;

enum class ParseEvent : std::uint8_t {
    NeedMore,         // all input consumed; feed more bytes
    HeadComplete,     // request()/response() is ready; body (if any) follows
    Body,             // ParseResult::body holds decoded body bytes (a view into the input)
    MessageComplete,  // message fully read; call reset() before the next one
    Error,            // error() describes it; the connection must not be reused
};

struct ParseError {
    int status = 0;  // HTTP status to answer with (400/431/501/505 for requests, 502 for responses)
    std::string_view detail;
};

struct ParseResult {
    ParseEvent event = ParseEvent::NeedMore;
    std::size_t consumed = 0;  // bytes of the input used; the rest belongs to the next call
    std::string_view body;     // Body event only
};

// Incremental HTTP/1.1 parser (plan IV.3). Pure transformation: bytes in, events out,
// no I/O. Messages may arrive split across any number of reads. Framing is strict
// (plan VII): ambiguous or malformed framing is an error, never guessed.
//
// Usage: call parse() with the unconsumed input repeatedly until it returns NeedMore
// (feed more data later), MessageComplete or Error. parse() stops at the end of a
// message, so pipelined bytes stay unconsumed for the next message after reset().
class HttpParser {
public:
    enum class Kind : std::uint8_t { Request, Response };

    HttpParser(Kind kind, const ParserLimits& limits) noexcept;

    // Prepares for the next message on the same connection.
    void reset();
    // Response parser only: a response to HEAD never has a body, whatever its headers say.
    void reset_for_response(bool request_was_head);

    ParseResult parse(std::string_view data);

    // The peer closed the connection. Returns MessageComplete if that ends a
    // close-delimited body, Error if a message was cut off, or NeedMore if no message
    // was in progress (a clean close between messages).
    ParseResult finish();

    // True if no byte of the current message has arrived yet.
    bool idle() const noexcept;

    const RequestHead& request() const noexcept { return request_; }
    const ResponseHead& response() const noexcept { return response_; }
    const ParseError& error() const noexcept { return error_; }

private:
    enum class State : std::uint8_t { Head, FixedBody, ChunkLine, ChunkData, ChunkDataEnd, Trailer, UntilClose, Done, Failed };

    ParseResult parse_head(std::string_view data);
    bool on_head_line_end();
    bool finish_head();
    bool parse_request_line(std::string_view line);
    bool parse_status_line(std::string_view line);
    bool parse_field_line(std::string_view line, Fields* out);
    bool decide_request_framing();
    bool decide_response_framing();
    bool parse_chunk_size_line();
    bool fail(int status, std::string_view detail);
    int head_status(int request_status) const noexcept { return kind_ == Kind::Request ? request_status : 502; }

    // Shared by chunk-size lines and trailer lines: strict CRLF line accumulation.
    // Returns how many bytes it consumed; sets *line_done when a full line is in line_.
    std::size_t take_line(std::string_view data, std::size_t max_bytes, int too_long_status, bool* line_done);

    Kind kind_;
    ParserLimits limits_;
    State state_ = State::Head;
    bool response_to_head_ = false;

    // Head accumulation.
    std::string head_;
    std::size_t start_line_end_ = std::string::npos;  // offset just past the start line's CRLF
    std::size_t line_start_ = 0;
    std::size_t field_count_ = 0;
    std::size_t skipped_crlf_bytes_ = 0;
    bool head_prev_cr_ = false;

    // Body state.
    std::uint64_t remaining_ = 0;
    std::string line_;  // current chunk-size or trailer line (without CRLF)
    bool line_prev_cr_ = false;
    std::size_t trailer_bytes_ = 0;
    std::size_t trailer_count_ = 0;
    std::size_t chunk_crlf_seen_ = 0;

    RequestHead request_;
    ResponseHead response_;
    ParseError error_;
};

}  // namespace lb::http
