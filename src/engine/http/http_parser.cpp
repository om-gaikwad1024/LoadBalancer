#include "http/http_parser.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace lb::http {

namespace {

constexpr std::string_view kCrlf = "\r\n";

char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// RFC 9110 tchar.
bool is_tchar(char c) noexcept {
    if (is_digit(c) || is_alpha(c)) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool is_token(std::string_view s) noexcept { return !s.empty() && std::all_of(s.begin(), s.end(), is_tchar); }

// field-vchar / obs-text / SP / HTAB: anything but control characters and DEL.
bool is_field_value_char(char ch) noexcept {
    const auto c = static_cast<unsigned char>(ch);
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

// Visible ASCII only; a fragment ('#') never belongs in a request target.
bool is_target_char(char ch) noexcept {
    const auto c = static_cast<unsigned char>(ch);
    return c > 0x20 && c < 0x7F && c != '#';
}

// reg-name / IP-literal / port characters (RFC 3986).
bool is_host_char(char c) noexcept {
    if (is_digit(c) || is_alpha(c)) return true;
    return std::string_view("-._~!$&'()*+,;=:%[]").find(c) != std::string_view::npos;
}

bool is_ows(char c) noexcept { return c == ' ' || c == '\t'; }

std::string_view trim_ows(std::string_view s) noexcept {
    while (!s.empty() && is_ows(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ows(s.back())) s.remove_suffix(1);
    return s;
}

int hex_value(char c) noexcept {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 1*DIGIT with overflow detection; no sign, no whitespace, no other characters.
std::optional<std::uint64_t> parse_decimal(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (char c : s) {
        if (!is_digit(c)) return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

template <typename Fn>
void for_each_list_element(std::string_view value, Fn&& fn) {
    while (true) {
        const auto comma = value.find(',');
        fn(trim_ows(value.substr(0, comma)));
        if (comma == std::string_view::npos) return;
        value.remove_prefix(comma + 1);
    }
}

bool starts_with_icase(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

enum class VersionCheck { Ok, Malformed, Unsupported };

VersionCheck parse_version(std::string_view s, Version* out) noexcept {
    if (s.size() != 8 || s.substr(0, 5) != "HTTP/" || !is_digit(s[5]) || s[6] != '.' || !is_digit(s[7])) {
        return VersionCheck::Malformed;
    }
    if (s[5] != '1') return VersionCheck::Unsupported;
    // RFC 9112 2.6: a higher 1.x minor version is processed as the highest we support.
    *out = s[7] == '0' ? Version::Http10 : Version::Http11;
    return VersionCheck::Ok;
}

enum class CodingCheck { Chunked, Malformed, Unsupported };

// Only exactly one "chunked" coding is accepted (plan VII: no lenient framing).
CodingCheck check_transfer_coding(const Fields& fields) {
    std::size_t chunked = 0;
    bool empty_element = false;
    bool unknown = false;
    for (const auto& f : fields.all()) {
        if (!iequals(f.name, "Transfer-Encoding")) continue;
        for_each_list_element(f.value, [&](std::string_view coding) {
            if (coding.empty()) empty_element = true;
            else if (iequals(coding, "chunked")) ++chunked;
            else unknown = true;
        });
    }
    if (empty_element) return CodingCheck::Malformed;
    if (unknown) return CodingCheck::Unsupported;
    return chunked == 1 ? CodingCheck::Chunked : CodingCheck::Malformed;
}

}  // namespace

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

const std::string* Fields::find(std::string_view name) const noexcept {
    for (const auto& f : fields_) {
        if (iequals(f.name, name)) return &f.value;
    }
    return nullptr;
}

std::size_t Fields::count(std::string_view name) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(fields_.begin(), fields_.end(), [name](const Field& f) { return iequals(f.name, name); }));
}

bool Fields::has_token(std::string_view name, std::string_view token) const {
    bool found = false;
    for (const auto& f : fields_) {
        if (!iequals(f.name, name)) continue;
        for_each_list_element(f.value, [&](std::string_view element) { found = found || iequals(element, token); });
    }
    return found;
}

ParserLimits request_parser_limits(const LimitsConfig& limits) noexcept {
    ParserLimits p;
    p.max_start_line_bytes = limits.max_request_line_bytes;
    p.max_field_section_bytes = limits.max_request_header_bytes;
    p.max_head_bytes = std::numeric_limits<std::size_t>::max();  // bounded by the two limits above
    p.max_field_count = limits.max_request_header_count;
    p.max_chunk_line_bytes = limits.max_chunk_line_bytes;
    return p;
}

ParserLimits response_parser_limits(const LimitsConfig& limits) noexcept {
    ParserLimits p;
    p.max_start_line_bytes = limits.max_response_header_bytes;
    p.max_field_section_bytes = limits.max_response_header_bytes;
    p.max_head_bytes = limits.max_response_header_bytes;
    p.max_field_count = limits.max_response_header_count;
    p.max_chunk_line_bytes = limits.max_chunk_line_bytes;
    return p;
}

HttpParser::HttpParser(Kind kind, const ParserLimits& limits) noexcept : kind_(kind), limits_(limits) {}

void HttpParser::reset() {
    state_ = State::Head;
    response_to_head_ = false;
    head_.clear();  // keeps capacity for the next message on this connection
    start_line_end_ = std::string::npos;
    line_start_ = 0;
    field_count_ = 0;
    skipped_crlf_bytes_ = 0;
    head_prev_cr_ = false;
    remaining_ = 0;
    line_.clear();
    line_prev_cr_ = false;
    trailer_bytes_ = 0;
    trailer_count_ = 0;
    chunk_crlf_seen_ = 0;
    request_ = {};
    response_ = {};
    error_ = {};
}

void HttpParser::reset_for_response(bool request_was_head) {
    reset();
    response_to_head_ = request_was_head;
}

bool HttpParser::idle() const noexcept { return state_ == State::Head && head_.empty(); }

bool HttpParser::fail(int status, std::string_view detail) {
    state_ = State::Failed;
    error_ = {status, detail};
    return false;
}

ParseResult HttpParser::parse(std::string_view data) {
    std::size_t pos = 0;
    for (;;) {
        switch (state_) {
            case State::Head:
                return parse_head(data);

            case State::FixedBody:
            case State::ChunkData: {
                if (pos == data.size()) return {ParseEvent::NeedMore, pos, {}};
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, data.size() - pos));
                remaining_ -= n;
                if (remaining_ == 0) {
                    if (state_ == State::FixedBody) {
                        state_ = State::Done;
                    } else {
                        state_ = State::ChunkDataEnd;
                        chunk_crlf_seen_ = 0;
                    }
                }
                return {ParseEvent::Body, pos + n, data.substr(pos, n)};
            }

            case State::ChunkLine: {
                bool done = false;
                pos += take_line(data.substr(pos), limits_.max_chunk_line_bytes, head_status(400), &done);
                if (state_ == State::Failed) break;
                if (!done) return {ParseEvent::NeedMore, pos, {}};
                parse_chunk_size_line();
                break;
            }

            case State::ChunkDataEnd:
                while (pos < data.size() && chunk_crlf_seen_ < kCrlf.size()) {
                    if (data[pos] != kCrlf[chunk_crlf_seen_]) {
                        fail(head_status(400), "chunk data not followed by CRLF");
                        break;
                    }
                    ++pos;
                    ++chunk_crlf_seen_;
                }
                if (state_ == State::Failed) break;
                if (chunk_crlf_seen_ < kCrlf.size()) return {ParseEvent::NeedMore, pos, {}};
                state_ = State::ChunkLine;
                break;

            case State::Trailer: {
                bool done = false;
                pos += take_line(data.substr(pos), limits_.max_field_section_bytes, head_status(431), &done);
                if (state_ == State::Failed) break;
                if (!done) return {ParseEvent::NeedMore, pos, {}};
                if (line_.empty()) {
                    state_ = State::Done;  // trailers are validated and dropped; the body is re-framed
                    break;
                }
                trailer_bytes_ += line_.size() + kCrlf.size();
                if (trailer_bytes_ > limits_.max_field_section_bytes) {
                    fail(head_status(431), "trailer section too large");
                } else if (++trailer_count_ > limits_.max_field_count) {
                    fail(head_status(431), "too many trailer fields");
                } else {
                    parse_field_line(line_, nullptr);
                }
                line_.clear();
                break;
            }

            case State::UntilClose:
                if (pos == data.size()) return {ParseEvent::NeedMore, pos, {}};
                return {ParseEvent::Body, data.size(), data.substr(pos)};

            case State::Done:
                return {ParseEvent::MessageComplete, pos, {}};

            case State::Failed:
                return {ParseEvent::Error, pos, {}};
        }
    }
}

ParseResult HttpParser::finish() {
    switch (state_) {
        case State::UntilClose:
            state_ = State::Done;
            return {ParseEvent::MessageComplete, 0, {}};
        case State::Done:
            return {ParseEvent::MessageComplete, 0, {}};
        case State::Failed:
            return {ParseEvent::Error, 0, {}};
        case State::Head:
            if (head_.empty()) return {ParseEvent::NeedMore, 0, {}};
            [[fallthrough]];
        default:
            fail(head_status(400), "connection closed before the message was complete");
            return {ParseEvent::Error, 0, {}};
    }
}

ParseResult HttpParser::parse_head(std::string_view data) {
    for (std::size_t i = 0; i < data.size(); ++i) {
        const char c = data[i];
        if (head_prev_cr_) {
            if (c != '\n') {
                fail(head_status(400), "CR not followed by LF in message head");
                return {ParseEvent::Error, i, {}};
            }
            head_prev_cr_ = false;
            head_ += c;
            if (!on_head_line_end()) return {ParseEvent::Error, i + 1, {}};
            if (state_ != State::Head) return {ParseEvent::HeadComplete, i + 1, {}};
            continue;
        }
        if (c == '\n') {
            fail(head_status(400), "bare LF in message head");
            return {ParseEvent::Error, i, {}};
        }
        // Control bytes never appear in a valid head (HTAB only inside fields and the
        // reason phrase, checked per line). Failing on the first one stops binary garbage,
        // such as a TLS ClientHello on the plain port, without waiting for a line end.
        if (const auto u = static_cast<unsigned char>(c); (u < 0x20 && c != '\r' && c != '\t') || u == 0x7F) {
            fail(head_status(400), "control character in message head");
            return {ParseEvent::Error, i, {}};
        }
        head_prev_cr_ = (c == '\r');
        head_ += c;

        if (start_line_end_ == std::string::npos) {
            if (head_.size() - (head_prev_cr_ ? 1 : 0) > limits_.max_start_line_bytes) {
                fail(head_status(400), kind_ == Kind::Request ? "request line too long" : "status line too long");
                return {ParseEvent::Error, i + 1, {}};
            }
        } else if (head_.size() - start_line_end_ > limits_.max_field_section_bytes + kCrlf.size()) {
            fail(head_status(431), "header section too large");
            return {ParseEvent::Error, i + 1, {}};
        }
        if (head_.size() > limits_.max_head_bytes) {
            fail(head_status(431), "message head too large");
            return {ParseEvent::Error, i + 1, {}};
        }
    }
    return {ParseEvent::NeedMore, data.size(), {}};
}

// Called right after a CRLF was appended to head_.
bool HttpParser::on_head_line_end() {
    if (start_line_end_ == std::string::npos) {
        if (head_ == kCrlf) {
            // RFC 9112 2.2: empty lines before a request line are ignored, within the line limit.
            if (kind_ == Kind::Response) return fail(502, "empty status line");
            skipped_crlf_bytes_ += kCrlf.size();
            head_.clear();
            if (skipped_crlf_bytes_ > limits_.max_start_line_bytes) return fail(400, "too many empty lines before request");
            return true;
        }
        start_line_end_ = head_.size();
        line_start_ = head_.size();
        return true;
    }

    if (head_.size() - line_start_ == kCrlf.size()) return finish_head();  // empty line: end of head

    if (head_.size() - start_line_end_ > limits_.max_field_section_bytes) {
        return fail(head_status(431), "header section too large");
    }
    if (++field_count_ > limits_.max_field_count) return fail(head_status(431), "too many header fields");
    line_start_ = head_.size();
    return true;
}

bool HttpParser::finish_head() {
    const std::string_view all(head_);
    const std::string_view start_line = all.substr(0, start_line_end_ - kCrlf.size());
    const bool start_ok = kind_ == Kind::Request ? parse_request_line(start_line) : parse_status_line(start_line);
    if (!start_ok) return false;

    Fields* fields = kind_ == Kind::Request ? &request_.fields : &response_.fields;
    const std::size_t end = all.size() - kCrlf.size();  // start of the final empty line
    for (std::size_t p = start_line_end_; p < end;) {
        const std::size_t eol = all.find(kCrlf, p);
        if (!parse_field_line(all.substr(p, eol - p), fields)) return false;
        p = eol + kCrlf.size();
    }

    const bool framing_ok = kind_ == Kind::Request ? decide_request_framing() : decide_response_framing();
    head_.clear();
    return framing_ok;
}

bool HttpParser::parse_request_line(std::string_view line) {
    for (char c : line) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return fail(400, "control character in request line");
    }
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return fail(400, "malformed request line");

    const std::string_view method = line.substr(0, sp1);
    const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);

    if (!is_token(method)) return fail(400, "invalid method");
    if (target.empty() || !std::all_of(target.begin(), target.end(), is_target_char)) {
        return fail(400, "invalid request target");
    }
    switch (parse_version(version, &request_.version)) {
        case VersionCheck::Ok: break;
        case VersionCheck::Malformed: return fail(400, "malformed HTTP version");
        case VersionCheck::Unsupported: return fail(505, "HTTP version not supported");
    }
    if (method == "CONNECT") return fail(501, "CONNECT is not supported");

    if (target == "*") {
        if (method != "OPTIONS") return fail(400, "asterisk-form target is only valid for OPTIONS");
    } else if (target.front() != '/') {
        // absolute-form (RFC 9112 3.2.2): scheme "://" non-empty authority
        const std::size_t scheme_len = starts_with_icase(target, "http://") ? 7 : starts_with_icase(target, "https://") ? 8 : 0;
        if (scheme_len == 0 || target.size() == scheme_len || target[scheme_len] == '/') {
            return fail(400, "invalid request target");
        }
    }
    request_.method = std::string(method);
    request_.target = std::string(target);
    return true;
}

bool HttpParser::parse_status_line(std::string_view line) {
    for (char c : line) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && c != '\t') || u == 0x7F) return fail(502, "control character in status line");
    }
    // HTTP-version SP 3DIGIT [ SP reason-phrase ]
    if (line.size() < 12 || line[8] != ' ' || !is_digit(line[9]) || !is_digit(line[10]) || !is_digit(line[11])) {
        return fail(502, "malformed status line");
    }
    if (parse_version(line.substr(0, 8), &response_.version) != VersionCheck::Ok) {
        return fail(502, "unsupported HTTP version in response");
    }
    const int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (status < 100 || status > 599) return fail(502, "invalid status code");
    if (line.size() > 12) {
        if (line[12] != ' ') return fail(502, "malformed status line");
        response_.reason = std::string(line.substr(13));
    }
    response_.status = status;
    return true;
}

bool HttpParser::parse_field_line(std::string_view line, Fields* out) {
    if (!line.empty() && is_ows(line.front())) return fail(head_status(400), "obsolete line folding");
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return fail(head_status(400), "malformed header field");
    const std::string_view name = line.substr(0, colon);
    if (!is_token(name)) return fail(head_status(400), "invalid header field name");
    const std::string_view value = trim_ows(line.substr(colon + 1));
    if (!std::all_of(value.begin(), value.end(), is_field_value_char)) {
        return fail(head_status(400), "invalid character in header field value");
    }
    if (out != nullptr) out->add(std::string(name), std::string(value));
    return true;
}

bool HttpParser::decide_request_framing() {
    RequestHead& r = request_;
    const Fields& f = r.fields;

    const std::size_t hosts = f.count("Host");
    if (hosts > 1) return fail(400, "multiple Host headers");
    if (hosts == 0 && r.version == Version::Http11) return fail(400, "missing Host header");
    if (hosts == 1) {
        const std::string& host = *f.find("Host");
        if (!std::all_of(host.begin(), host.end(), is_host_char)) return fail(400, "invalid Host header");
    }

    const std::size_t te = f.count("Transfer-Encoding");
    const std::size_t cl = f.count("Content-Length");
    if (te > 0 && cl > 0) return fail(400, "both Content-Length and Transfer-Encoding");
    if (te > 0) {
        if (r.version == Version::Http10) return fail(400, "Transfer-Encoding in an HTTP/1.0 request");
        switch (check_transfer_coding(f)) {
            case CodingCheck::Chunked: break;
            case CodingCheck::Malformed: return fail(400, "malformed Transfer-Encoding");
            case CodingCheck::Unsupported: return fail(501, "unsupported transfer coding");
        }
        r.framing = BodyFraming::Chunked;
        state_ = State::ChunkLine;
    } else if (cl > 0) {
        if (cl > 1) return fail(400, "multiple Content-Length headers");
        const auto length = parse_decimal(*f.find("Content-Length"));
        if (!length) return fail(400, "invalid Content-Length");
        r.framing = BodyFraming::ContentLength;
        r.content_length = *length;
        remaining_ = *length;
        state_ = *length == 0 ? State::Done : State::FixedBody;
    } else {
        r.framing = BodyFraming::None;
        state_ = State::Done;
    }

    r.keep_alive = !f.has_token("Connection", "close") &&
                   (r.version == Version::Http11 || f.has_token("Connection", "keep-alive"));
    return true;
}

bool HttpParser::decide_response_framing() {
    ResponseHead& r = response_;
    const Fields& f = r.fields;

    if (r.status == 101) return fail(502, "protocol upgrade is not supported");

    // RFC 9112 6.3: these responses never have a body, whatever their headers say.
    const bool no_body = response_to_head_ || r.status < 200 || r.status == 204 || r.status == 304;
    const std::size_t te = f.count("Transfer-Encoding");
    const std::size_t cl = f.count("Content-Length");

    if (no_body) {
        r.framing = BodyFraming::None;
        state_ = State::Done;
    } else if (te > 0 && cl > 0) {
        return fail(502, "both Content-Length and Transfer-Encoding in response");
    } else if (te > 0) {
        if (check_transfer_coding(f) != CodingCheck::Chunked) return fail(502, "unsupported Transfer-Encoding in response");
        r.framing = BodyFraming::Chunked;
        state_ = State::ChunkLine;
    } else if (cl > 0) {
        if (cl > 1) return fail(502, "multiple Content-Length headers in response");
        const auto length = parse_decimal(*f.find("Content-Length"));
        if (!length) return fail(502, "invalid Content-Length in response");
        r.framing = BodyFraming::ContentLength;
        r.content_length = *length;
        remaining_ = *length;
        state_ = *length == 0 ? State::Done : State::FixedBody;
    } else {
        r.framing = BodyFraming::UntilClose;
        state_ = State::UntilClose;
    }

    r.keep_alive = r.framing != BodyFraming::UntilClose && !f.has_token("Connection", "close") &&
                   (r.version == Version::Http11 || f.has_token("Connection", "keep-alive"));
    return true;
}

bool HttpParser::parse_chunk_size_line() {
    const std::string_view line(line_);
    std::uint64_t size = 0;
    std::size_t i = 0;
    for (; i < line.size(); ++i) {
        const int digit = hex_value(line[i]);
        if (digit < 0) break;
        if (size > (std::numeric_limits<std::uint64_t>::max() >> 4)) return fail(head_status(400), "chunk size overflow");
        size = (size << 4) | static_cast<std::uint64_t>(digit);
    }
    if (i == 0) return fail(head_status(400), "invalid chunk size");

    // Optional chunk extensions: BWS ";" ... (no control characters).
    std::string_view rest = line.substr(i);
    if (!rest.empty()) {
        while (!rest.empty() && is_ows(rest.front())) rest.remove_prefix(1);
        if (rest.empty() || rest.front() != ';') return fail(head_status(400), "invalid chunk size line");
        if (!std::all_of(rest.begin(), rest.end(), is_field_value_char)) {
            return fail(head_status(400), "invalid chunk extension");
        }
    }

    line_.clear();
    if (size == 0) {
        state_ = State::Trailer;
    } else {
        remaining_ = size;
        state_ = State::ChunkData;
    }
    return true;
}

std::size_t HttpParser::take_line(std::string_view data, std::size_t max_bytes, int too_long_status, bool* line_done) {
    for (std::size_t i = 0; i < data.size(); ++i) {
        const char c = data[i];
        if (line_prev_cr_) {
            line_prev_cr_ = false;
            if (c != '\n') {
                fail(head_status(400), "CR not followed by LF");
                return i;
            }
            *line_done = true;
            return i + 1;
        }
        if (c == '\r') {
            line_prev_cr_ = true;
            continue;
        }
        if (c == '\n') {
            fail(head_status(400), "bare LF");
            return i;
        }
        if (line_.size() >= max_bytes) {
            fail(too_long_status, "line too long");
            return i;
        }
        line_ += c;
    }
    return data.size();
}

}  // namespace lb::http
