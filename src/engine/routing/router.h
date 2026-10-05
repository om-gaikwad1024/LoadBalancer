#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "config/config.h"
#include "http/http_message.h"

namespace lb::routing {

// Content-aware routing (plan IV.8): picks the group for every request, before session
// affinity and load balancing (plan III). Pure functions over the request head and the
// request's config snapshot: no state, no lock, no allocation.

// The path of a request target: origin-form "/a/b?q" gives "/a/b"; absolute-form
// "http://host/a?q" gives "/a" ("/" if it has no path); "*" stays "*". Matching uses the
// path exactly as sent: no percent-decoding and no dot-segment removal.
std::string_view request_path(std::string_view target) noexcept;

// "/api" matches "/api", "/api/" and "/api/v1", not "/apiary". A prefix ending in '/'
// matches anything under it.
bool path_prefix_matches(std::string_view path, std::string_view prefix) noexcept;

// The whole text matches the pattern; '*' matches any run of characters (including '/').
// At worst O(text x pattern), never exponential: only the last '*' is ever backtracked.
bool glob_matches(std::string_view text, std::string_view pattern) noexcept;

// Any field named `name` (case-insensitive) whose value equals `value` exactly, or any such
// field at all when `value` is empty.
bool header_matches(const http::Fields& fields, std::string_view name, const std::optional<std::string>& value) noexcept;

// Any cookie in any Cookie header named `name` (case-sensitive) whose value, without
// surrounding double quotes, equals `value`; or any such cookie when `value` is empty.
bool cookie_matches(const http::Fields& fields, std::string_view name, const std::optional<std::string>& value) noexcept;

bool rule_matches(const RouteRule& rule, std::string_view path, const http::Fields& fields) noexcept;

struct RouteDecision {
    const std::string* group = nullptr;  // never null: the matched rule's group or the default
    const RouteRule* rule = nullptr;     // null: no rule matched, default group
};

// The first matching rule in order, or the default group.
RouteDecision route(const RoutingConfig& routing, const http::RequestHead& request) noexcept;

}  // namespace lb::routing
