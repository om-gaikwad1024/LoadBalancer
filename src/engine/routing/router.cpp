#include "routing/router.h"

#include <cctype>

namespace lb::routing {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::string_view request_path(std::string_view target) noexcept {
    if (target == "*") return target;
    if (!target.empty() && target.front() != '/') {  // absolute-form: skip scheme://authority
        const auto scheme_end = target.find("://");
        if (scheme_end == std::string_view::npos) return target.substr(0, target.find_first_of("?#"));
        const auto path_start = target.find('/', scheme_end + 3);
        if (path_start == std::string_view::npos) return "/";
        target.remove_prefix(path_start);
    }
    return target.substr(0, target.find_first_of("?#"));
}

bool path_prefix_matches(std::string_view path, std::string_view prefix) noexcept {
    if (path.substr(0, prefix.size()) != prefix) return false;
    return path.size() == prefix.size() || prefix.back() == '/' || path[prefix.size()] == '/';
}

bool glob_matches(std::string_view text, std::string_view pattern) noexcept {
    std::size_t t = 0;
    std::size_t p = 0;
    std::size_t star = std::string_view::npos;  // position of the last '*' in the pattern
    std::size_t resume = 0;                     // text position that '*' is currently extended to
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;  // let the last '*' absorb one more character
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool header_matches(const http::Fields& fields, std::string_view name, const std::optional<std::string>& value) noexcept {
    for (const auto& f : fields.all()) {
        if (!iequals(f.name, name)) continue;
        if (!value || trim(f.value) == *value) return true;
    }
    return false;
}

bool cookie_matches(const http::Fields& fields, std::string_view name, const std::optional<std::string>& value) noexcept {
    for (const auto& f : fields.all()) {
        if (!iequals(f.name, "Cookie")) continue;
        std::string_view rest = f.value;
        while (!rest.empty()) {
            const auto semi = rest.find(';');
            const std::string_view pair = trim(rest.substr(0, semi));
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            const auto eq = pair.find('=');
            if (eq == std::string_view::npos || trim(pair.substr(0, eq)) != name) continue;
            if (!value) return true;
            std::string_view v = trim(pair.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            if (v == *value) return true;
        }
    }
    return false;
}

bool rule_matches(const RouteRule& rule, std::string_view path, const http::Fields& fields) noexcept {
    switch (rule.type) {
        case RouteRule::Type::PathPrefix: return rule.value && path_prefix_matches(path, *rule.value);
        case RouteRule::Type::PathGlob: return rule.value && glob_matches(path, *rule.value);
        case RouteRule::Type::Header: return header_matches(fields, rule.field, rule.value);
        case RouteRule::Type::Cookie: return cookie_matches(fields, rule.field, rule.value);
    }
    return false;
}

RouteDecision route(const RoutingConfig& routing, const http::RequestHead& request) noexcept {
    const std::string_view path = request_path(request.target);
    for (const auto& rule : routing.rules) {
        if (rule_matches(rule, path, request.fields)) return {&rule.group, &rule};
    }
    return {&routing.default_group, nullptr};
}

}  // namespace lb::routing
