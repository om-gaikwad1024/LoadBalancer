// Step 2.3 (plan IV.8): content-aware routing rule matching.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "routing/router.h"

using lb::RouteRule;
using lb::routing::cookie_matches;
using lb::routing::glob_matches;
using lb::routing::header_matches;
using lb::routing::path_prefix_matches;
using lb::routing::request_path;

namespace {

lb::http::Fields fields(std::vector<std::pair<std::string, std::string>> list) {
    lb::http::Fields f;
    for (auto& [name, value] : list) f.add(std::move(name), std::move(value));
    return f;
}

lb::http::RequestHead request(std::string target, std::vector<std::pair<std::string, std::string>> headers = {}) {
    lb::http::RequestHead r;
    r.method = "GET";
    r.target = std::move(target);
    r.fields = fields(std::move(headers));
    return r;
}

RouteRule rule(std::string id, RouteRule::Type type, std::string field, std::optional<std::string> value,
               std::string group) {
    RouteRule r;
    r.id = std::move(id);
    r.type = type;
    r.field = std::move(field);
    r.value = std::move(value);
    r.group = std::move(group);
    return r;
}

lb::RoutingConfig routing() {
    lb::RoutingConfig c;
    c.default_group = "web";
    c.rules = {
        rule("api", RouteRule::Type::PathPrefix, "", "/api", "api"),
        rule("reports", RouteRule::Type::PathGlob, "", "/reports/*.csv", "reports"),
        rule("beta", RouteRule::Type::Header, "X-Beta", "on", "beta"),
        rule("canary", RouteRule::Type::Cookie, "canary", std::nullopt, "canary"),
        rule("api-again", RouteRule::Type::PathPrefix, "", "/api/v2", "never"),  // shadowed by "api"
    };
    return c;
}

std::string routed(const lb::http::RequestHead& r) {
    const auto config = routing();
    return *lb::routing::route(config, r).group;
}

}  // namespace

TEST(RequestPath, StripsQueryFragmentAndAuthority) {
    EXPECT_EQ(request_path("/a/b?x=1"), "/a/b");
    EXPECT_EQ(request_path("/a/b#frag"), "/a/b");
    EXPECT_EQ(request_path("/"), "/");
    EXPECT_EQ(request_path("http://example.com:8080/api/x?y"), "/api/x");
    EXPECT_EQ(request_path("http://example.com"), "/");
    EXPECT_EQ(request_path("*"), "*");
    EXPECT_EQ(request_path("/%61pi"), "/%61pi");  // no decoding: matched as sent
}

TEST(PathPrefix, MatchesWholeSegmentsOnly) {
    EXPECT_TRUE(path_prefix_matches("/api", "/api"));
    EXPECT_TRUE(path_prefix_matches("/api/", "/api"));
    EXPECT_TRUE(path_prefix_matches("/api/v1/users", "/api"));
    EXPECT_FALSE(path_prefix_matches("/apiary", "/api"));
    EXPECT_FALSE(path_prefix_matches("/ap", "/api"));
    EXPECT_FALSE(path_prefix_matches("/API", "/api"));  // case-sensitive
    EXPECT_TRUE(path_prefix_matches("/api/x", "/api/"));
    EXPECT_FALSE(path_prefix_matches("/api", "/api/"));
    EXPECT_TRUE(path_prefix_matches("/anything", "/"));
}

TEST(PathGlob, StarMatchesAnyRun) {
    EXPECT_TRUE(glob_matches("/reports/q1.csv", "/reports/*.csv"));
    EXPECT_TRUE(glob_matches("/reports/2026/q1.csv", "/reports/*.csv"));  // '*' crosses '/'
    EXPECT_TRUE(glob_matches("/reports/.csv", "/reports/*.csv"));
    EXPECT_FALSE(glob_matches("/reports/q1.csv.bak", "/reports/*.csv"));  // the whole path must match
    EXPECT_FALSE(glob_matches("/other/q1.csv", "/reports/*.csv"));
    EXPECT_TRUE(glob_matches("/a/b/c", "/*/*/*"));
    EXPECT_TRUE(glob_matches("/x", "*"));
    EXPECT_TRUE(glob_matches("/exact", "/exact"));
    EXPECT_FALSE(glob_matches("/exact", "/exac"));
    EXPECT_TRUE(glob_matches("/a-b-c-d", "/a*b*c*d"));
    EXPECT_FALSE(glob_matches("/a-b-c", "/a*b*c*d"));
}

TEST(PathGlob, HostilePatternStaysFast) {
    // Exponential in a naive recursive matcher; here at most text x pattern steps.
    const std::string text(8000, 'a');
    const std::string pattern = "/" + std::string(1000, '*') + "b";
    EXPECT_FALSE(glob_matches(text, pattern));
    EXPECT_FALSE(glob_matches(text, "*a*a*a*a*a*a*a*a*a*a*b"));
}

TEST(HeaderRule, ValueOrPresence) {
    const auto f = fields({{"x-beta", " on "}, {"Accept", "*/*"}});
    EXPECT_TRUE(header_matches(f, "X-Beta", std::string("on")));  // name case-insensitive, value trimmed
    EXPECT_FALSE(header_matches(f, "X-Beta", std::string("ON")));  // value case-sensitive
    EXPECT_TRUE(header_matches(f, "X-Beta", std::nullopt));
    EXPECT_FALSE(header_matches(f, "X-Gamma", std::nullopt));
    // Repeated header: any line may match.
    const auto twice = fields({{"X-Beta", "off"}, {"X-Beta", "on"}});
    EXPECT_TRUE(header_matches(twice, "X-Beta", std::string("on")));
}

TEST(CookieRule, ParsesCookieHeaders) {
    const auto f = fields({{"Cookie", "session=abc; canary=1;theme=\"dark\""}, {"cookie", "late=yes"}});
    EXPECT_TRUE(cookie_matches(f, "canary", std::nullopt));
    EXPECT_TRUE(cookie_matches(f, "canary", std::string("1")));
    EXPECT_FALSE(cookie_matches(f, "canary", std::string("2")));
    EXPECT_TRUE(cookie_matches(f, "theme", std::string("dark")));  // quotes removed
    EXPECT_TRUE(cookie_matches(f, "late", std::nullopt));          // a second Cookie header line
    EXPECT_FALSE(cookie_matches(f, "Canary", std::nullopt));       // names are case-sensitive
    EXPECT_FALSE(cookie_matches(f, "sess", std::nullopt));
    EXPECT_FALSE(cookie_matches(fields({{"X-Cookie", "canary=1"}}), "canary", std::nullopt));
    EXPECT_FALSE(cookie_matches(fields({{"Cookie", "canary"}}), "canary", std::nullopt));  // no '='
}

// Plan IV.8 done (rule level): requests that differ only in path, header or cookie are
// routed to different groups.
TEST(Route, RequestsDifferingOnlyInOneAttributeGoToDifferentGroups) {
    EXPECT_EQ(routed(request("/home")), "web");
    EXPECT_EQ(routed(request("/api/users")), "api");
    EXPECT_EQ(routed(request("/reports/q1.csv")), "reports");
    EXPECT_EQ(routed(request("/home", {{"X-Beta", "on"}})), "beta");
    EXPECT_EQ(routed(request("/home", {{"X-Beta", "off"}})), "web");
    EXPECT_EQ(routed(request("/home", {{"Cookie", "canary=yes"}})), "canary");
    EXPECT_EQ(routed(request("/home", {{"Cookie", "other=yes"}})), "web");
}

TEST(Route, FirstMatchingRuleWins) {
    // Path rule listed before the header rule.
    EXPECT_EQ(routed(request("/api/x", {{"X-Beta", "on"}})), "api");
    // "api" (/api) also covers /api/v2, so the later "api-again" rule never wins.
    const auto config = routing();  // the decision points into the config
    const auto d = lb::routing::route(config, request("/api/v2/x"));
    EXPECT_EQ(*d.group, "api");
    ASSERT_NE(d.rule, nullptr);
    EXPECT_EQ(d.rule->id, "api");
}

TEST(Route, NoRuleMeansDefaultGroup) {
    const auto config = routing();
    const auto d = lb::routing::route(config, request("/"));
    EXPECT_EQ(*d.group, "web");
    EXPECT_EQ(d.rule, nullptr);
    lb::RoutingConfig empty;
    empty.default_group = "only";
    EXPECT_EQ(*lb::routing::route(empty, request("/api")).group, "only");
}

TEST(Route, QueryStringIsNotPartOfThePath) {
    EXPECT_EQ(routed(request("/home?path=/api")), "web");
    EXPECT_EQ(routed(request("/api?x=1")), "api");
    EXPECT_EQ(routed(request("http://h/api/x")), "api");
}
