#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lb::http {

enum class Version : std::uint8_t { Http10, Http11 };

// How the body of a message is delimited (plan IV.3).
enum class BodyFraming : std::uint8_t {
    None,           // no body (or Content-Length: 0)
    ContentLength,  // exactly content_length bytes
    Chunked,        // Transfer-Encoding: chunked
    UntilClose,     // response only: body ends when the backend closes the connection
};

bool iequals(std::string_view a, std::string_view b) noexcept;

struct Field {
    std::string name;  // as received (case preserved)
    std::string value;  // leading/trailing whitespace removed
};

// Header fields in arrival order. Lookups are case-insensitive.
class Fields {
public:
    void add(std::string name, std::string value) { fields_.push_back({std::move(name), std::move(value)}); }
    void clear() noexcept { fields_.clear(); }

    const std::vector<Field>& all() const noexcept { return fields_; }
    std::size_t size() const noexcept { return fields_.size(); }

    // First value of the named field, or nullptr.
    const std::string* find(std::string_view name) const noexcept;
    std::size_t count(std::string_view name) const noexcept;
    // True if any field with this name lists `token` in its comma-separated value (case-insensitive).
    bool has_token(std::string_view name, std::string_view token) const;

private:
    std::vector<Field> fields_;
};

struct RequestHead {
    std::string method;
    std::string target;
    Version version = Version::Http11;
    Fields fields;
    BodyFraming framing = BodyFraming::None;
    std::uint64_t content_length = 0;  // valid when framing == ContentLength
    bool keep_alive = false;           // client connection may carry another request afterwards
};

struct ResponseHead {
    Version version = Version::Http11;
    int status = 0;
    std::string reason;
    Fields fields;
    BodyFraming framing = BodyFraming::None;
    std::uint64_t content_length = 0;
    // Backend connection may be reused (returned to the pool) once this response is fully read.
    bool keep_alive = false;
};

}  // namespace lb::http
