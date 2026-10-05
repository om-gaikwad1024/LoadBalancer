#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config/config_loader.h"

using json = nlohmann::json;
using lb::ConfigLoadResult;

namespace {

json good_config() {
    return json::parse(R"({
      "listen":  { "address": "0.0.0.0", "port": 8080, "backlog": 512, "pending_accepts": 16 },
      "workers": { "threads": 4 },
      "limits": {
        "max_request_line_bytes": 8192, "max_request_header_bytes": 32768, "max_request_header_count": 100,
        "max_response_header_bytes": 65536, "max_response_header_count": 200, "max_chunk_line_bytes": 4096,
        "max_client_connections": 10000 },
      "buffers": { "client_read_bytes": 16384, "backend_read_bytes": 8192 },
      "pool": { "max_connections_per_backend": 128, "max_idle_per_backend": 32, "idle_timeout_ms": 30000,
                "max_waiters_per_backend": 500, "wait_timeout_ms": 1000, "fail_fast_connect": false },
      "maintenance": { "interval_ms": 250 },
      "metrics": { "slice_ms": 1000, "window_slices": 10, "max_backend_series": 64 },
      "config_reload": { "watch_file": true, "debounce_ms": 250 },
      "balancing": { "response_time_decay_ms": 5000, "response_time_expiry_ms": 20000 },
      "sticky_table": { "shards": 16, "max_entries": 50000 },
      "dashboard": { "publish_interval_ms": 250, "event_rows": 300, "graph_points": 90 },
      "event_log": { "path": "logs/events.jsonl", "max_file_bytes": 1048576, "max_files": 3, "max_queue": 1000,
                     "recent_events": 200, "trace_requests": true },
      "timeouts": { "client_header_ms": 10000, "client_body_idle_ms": 20000, "client_keepalive_idle_ms": 30000,
                    "client_write_idle_ms": 40000, "backend_connect_ms": 3000, "backend_response_ms": 50000,
                    "backend_idle_ms": 60000, "shutdown_grace_ms": 5000, "drain_ms": 45000 },
      "trusted_proxies": [ "10.0.0.0/8", "192.168.1.7" ],
      "groups": [
        { "name": "web", "strategy": "round_robin", "host_header": "preserve",
          "sticky": { "mode": "inserted_cookie", "cookie": "lb_session", "ttl_ms": 600000 },
          "passive_health": { "enabled": true, "consecutive_failures": 7, "count_5xx": true },
          "health": { "type": "http", "path": "/healthz", "interval_ms": 2000, "timeout_ms": 500,
                      "unhealthy_threshold": 3, "healthy_threshold": 2 },
          "backends": [
            { "id": "web-1", "address": "127.0.0.1", "port": 9001, "weight": 3, "drain": "keep" },
            { "id": "web-2", "address": "127.0.0.2", "port": 9002, "weight": 1, "drain": "start" } ] },
        { "name": "api", "strategy": "least_connections", "host_header": "backend",
          "sticky": { "mode": "off", "cookie": null, "ttl_ms": 1000 },
          "passive_health": { "enabled": false, "consecutive_failures": 1, "count_5xx": false },
          "health": { "type": "tcp", "path": "/", "interval_ms": 1000, "timeout_ms": 1000,
                      "unhealthy_threshold": 1, "healthy_threshold": 5 },
          "backends": [
            { "id": "api-1", "address": "10.0.0.5", "port": 7000, "weight": 1, "drain": "cancel" } ] }
      ],
      "routing": { "default_group": "web", "rules": [
        { "id": "api-path", "type": "path_prefix", "field": null, "value": "/api", "group": "api" },
        { "id": "static", "type": "path_glob", "field": null, "value": "/static/*.css", "group": "web" },
        { "id": "beta-header", "type": "header", "field": "X-Beta", "value": "on", "group": "api" },
        { "id": "any-canary", "type": "cookie", "field": "canary", "value": null, "group": "api" } ] }
    })");
}

ConfigLoadResult load(const json& j) { return lb::parse_config(j.dump()); }

std::string describe(const ConfigLoadResult& r) {
    std::string out;
    for (const auto& e : r.errors) out += "  " + lb::to_string(e) + "\n";
    return out.empty() ? "  (no errors)\n" : out;
}

bool has_error(const ConfigLoadResult& r, std::string_view path, std::string_view fragment) {
    for (const auto& e : r.errors) {
        if (e.path == path && e.message.find(fragment) != std::string::npos) return true;
    }
    return false;
}

#define EXPECT_REJECTED_WITH(result, path, fragment)                                               \
    do {                                                                                           \
        const auto& r_ = (result);                                                                 \
        EXPECT_FALSE(r_.ok());                                                                     \
        EXPECT_EQ(r_.snapshot, nullptr);                                                           \
        EXPECT_TRUE(has_error(r_, path, fragment))                                                 \
            << "expected error at '" << (path) << "' containing '" << (fragment) << "', got:\n"  \
            << describe(r_);                                                                       \
    } while (0)

json with(json j, const char* pointer, json value) {
    j[json::json_pointer(pointer)] = std::move(value);
    return j;
}

// Every object member in the document, depth-first (array elements are traversed, not listed).
void collect_members(const json& j, const json::json_pointer& at, std::vector<json::json_pointer>& out) {
    if (j.is_object()) {
        for (const auto& item : j.items()) {
            const auto p = at / item.key();
            out.push_back(p);
            collect_members(item.value(), p, out);
        }
    } else if (j.is_array()) {
        for (std::size_t i = 0; i < j.size(); ++i) collect_members(j[i], at / i, out);
    }
}

}  // namespace

// ---- Accepts good configs -------------------------------------------------------------

TEST(ConfigLoader, AcceptsGoodConfigAndMapsEveryField) {
    const auto r = load(good_config());
    ASSERT_TRUE(r.ok()) << describe(r);
    EXPECT_TRUE(r.errors.empty());

    const lb::ConfigSnapshot& c = *r.snapshot;
    EXPECT_EQ(c.listen.address, "0.0.0.0");
    EXPECT_EQ(c.listen.port, 8080);
    ASSERT_TRUE(c.workers.threads.has_value());
    EXPECT_EQ(*c.workers.threads, 4u);
    EXPECT_EQ(c.limits.max_request_line_bytes, 8192u);
    EXPECT_EQ(c.limits.max_request_header_bytes, 32768u);
    EXPECT_EQ(c.limits.max_request_header_count, 100u);
    EXPECT_EQ(c.limits.max_response_header_bytes, 65536u);
    EXPECT_EQ(c.limits.max_response_header_count, 200u);
    EXPECT_EQ(c.limits.max_chunk_line_bytes, 4096u);
    EXPECT_EQ(c.limits.max_client_connections, 10000u);
    EXPECT_EQ(c.listen.backlog, 512u);
    EXPECT_EQ(c.listen.pending_accepts, 16u);
    EXPECT_EQ(c.buffers.client_read_bytes, 16384u);
    EXPECT_EQ(c.buffers.backend_read_bytes, 8192u);
    EXPECT_EQ(c.timeouts.shutdown_grace_ms, 5000u);
    EXPECT_EQ(c.pool.max_connections_per_backend, 128u);
    EXPECT_EQ(c.pool.max_idle_per_backend, 32u);
    EXPECT_EQ(c.pool.idle_timeout_ms, 30000u);
    EXPECT_EQ(c.pool.max_waiters_per_backend, 500u);
    EXPECT_EQ(c.pool.wait_timeout_ms, 1000u);
    EXPECT_FALSE(c.pool.fail_fast_connect);
    EXPECT_EQ(c.maintenance.interval_ms, 250u);
    EXPECT_EQ(c.metrics.slice_ms, 1000u);
    EXPECT_EQ(c.metrics.window_slices, 10u);
    EXPECT_EQ(c.metrics.max_backend_series, 64u);
    EXPECT_TRUE(c.config_reload.watch_file);
    EXPECT_EQ(c.config_reload.debounce_ms, 250u);
    EXPECT_EQ(c.balancing.response_time_decay_ms, 5000u);
    EXPECT_EQ(c.balancing.response_time_expiry_ms, 20000u);
    EXPECT_EQ(c.sticky_table.shards, 16u);
    EXPECT_EQ(c.sticky_table.max_entries, 50000u);
    EXPECT_EQ(c.groups[0].sticky.mode, lb::StickyConfig::Mode::InsertedCookie);
    EXPECT_EQ(c.groups[0].sticky.cookie, "lb_session");
    EXPECT_EQ(c.groups[0].sticky.ttl_ms, 600000u);
    EXPECT_EQ(c.groups[1].sticky.mode, lb::StickyConfig::Mode::Off);
    EXPECT_TRUE(c.groups[1].sticky.cookie.empty());
    EXPECT_EQ(c.timeouts.drain_ms, 45000u);
    EXPECT_EQ(c.groups[0].backends[0].drain, lb::DrainDirective::Keep);
    EXPECT_EQ(c.groups[0].backends[1].drain, lb::DrainDirective::Start);
    EXPECT_EQ(c.groups[1].backends[0].drain, lb::DrainDirective::Cancel);
    EXPECT_TRUE(c.groups[0].passive_health.enabled);
    EXPECT_EQ(c.groups[0].passive_health.consecutive_failures, 7u);
    EXPECT_TRUE(c.groups[0].passive_health.count_5xx);
    EXPECT_FALSE(c.groups[1].passive_health.enabled);
    EXPECT_EQ(c.dashboard.publish_interval_ms, 250u);
    EXPECT_EQ(c.dashboard.event_rows, 300u);
    EXPECT_EQ(c.event_log.path, "logs/events.jsonl");
    EXPECT_EQ(c.event_log.max_file_bytes, 1048576u);
    EXPECT_EQ(c.event_log.max_files, 3u);
    EXPECT_EQ(c.event_log.max_queue, 1000u);
    EXPECT_EQ(c.event_log.recent_events, 200u);
    EXPECT_TRUE(c.event_log.trace_requests);
    EXPECT_EQ(c.timeouts.client_header_ms, 10000u);
    EXPECT_EQ(c.timeouts.client_body_idle_ms, 20000u);
    EXPECT_EQ(c.timeouts.client_keepalive_idle_ms, 30000u);
    EXPECT_EQ(c.timeouts.client_write_idle_ms, 40000u);
    EXPECT_EQ(c.timeouts.backend_connect_ms, 3000u);
    EXPECT_EQ(c.timeouts.backend_response_ms, 50000u);
    EXPECT_EQ(c.timeouts.backend_idle_ms, 60000u);
    EXPECT_EQ(c.groups[0].health.type, lb::HealthConfig::Type::Http);
    EXPECT_EQ(c.groups[0].health.path, "/healthz");
    EXPECT_EQ(c.groups[0].health.interval_ms, 2000u);
    EXPECT_EQ(c.groups[0].health.timeout_ms, 500u);
    EXPECT_EQ(c.groups[0].health.unhealthy_threshold, 3u);
    EXPECT_EQ(c.groups[0].health.healthy_threshold, 2u);
    EXPECT_EQ(c.groups[1].health.type, lb::HealthConfig::Type::Tcp);
    EXPECT_EQ(c.groups[1].health.healthy_threshold, 5u);
    EXPECT_EQ(c.groups[0].strategy, lb::Strategy::RoundRobin);
    EXPECT_EQ(c.groups[1].strategy, lb::Strategy::LeastConnections);
    EXPECT_EQ(c.groups[0].host_header, lb::HostHeaderMode::Preserve);
    EXPECT_EQ(c.groups[1].host_header, lb::HostHeaderMode::Backend);
    ASSERT_EQ(c.trusted_proxies.size(), 2u);
    EXPECT_TRUE(c.is_trusted_proxy(0x0A010203));   // 10.1.2.3
    EXPECT_TRUE(c.is_trusted_proxy(0xC0A80107));   // 192.168.1.7
    EXPECT_FALSE(c.is_trusted_proxy(0xC0A80108));  // 192.168.1.8
    EXPECT_FALSE(c.is_trusted_proxy(0x0B000001));  // 11.0.0.1
    ASSERT_EQ(c.groups.size(), 2u);
    EXPECT_EQ(c.groups[0].name, "web");
    ASSERT_EQ(c.groups[0].backends.size(), 2u);
    EXPECT_EQ(c.groups[0].backends[0].id, "web-1");
    EXPECT_EQ(c.groups[0].backends[0].address, "127.0.0.1");
    EXPECT_EQ(c.groups[0].backends[0].port, 9001);
    EXPECT_EQ(c.groups[0].backends[0].weight, 3u);
    EXPECT_EQ(c.groups[1].backends[0].id, "api-1");
    EXPECT_EQ(c.routing.default_group, "web");
    ASSERT_EQ(c.routing.rules.size(), 4u);
    EXPECT_EQ(c.routing.rules[0].id, "api-path");
    EXPECT_EQ(c.routing.rules[0].type, lb::RouteRule::Type::PathPrefix);
    EXPECT_TRUE(c.routing.rules[0].field.empty());
    EXPECT_EQ(c.routing.rules[0].value, "/api");
    EXPECT_EQ(c.routing.rules[0].group, "api");
    EXPECT_EQ(c.routing.rules[1].type, lb::RouteRule::Type::PathGlob);
    EXPECT_EQ(c.routing.rules[1].value, "/static/*.css");
    EXPECT_EQ(c.routing.rules[2].type, lb::RouteRule::Type::Header);
    EXPECT_EQ(c.routing.rules[2].field, "X-Beta");
    EXPECT_EQ(c.routing.rules[2].value, "on");
    EXPECT_EQ(c.routing.rules[3].type, lb::RouteRule::Type::Cookie);
    EXPECT_EQ(c.routing.rules[3].field, "canary");
    EXPECT_FALSE(c.routing.rules[3].value.has_value());
    ASSERT_NE(c.find_group("api"), nullptr);
    EXPECT_EQ(c.find_group("nope"), nullptr);
}

TEST(ConfigLoader, ThreadsAutoMeansUnresolved) {
    const auto r = load(with(good_config(), "/workers/threads", "auto"));
    ASSERT_TRUE(r.ok()) << describe(r);
    EXPECT_FALSE(r.snapshot->workers.threads.has_value());
}

TEST(ConfigLoader, EveryStrategyNameMaps) {
    const std::pair<const char*, lb::Strategy> names[] = {
        {"round_robin", lb::Strategy::RoundRobin},
        {"least_connections", lb::Strategy::LeastConnections},
        {"weighted_round_robin", lb::Strategy::WeightedRoundRobin},
        {"least_response_time", lb::Strategy::LeastResponseTime},
        {"ip_hash", lb::Strategy::IpHash},
    };
    for (const auto& [name, strategy] : names) {
        const auto r = load(with(good_config(), "/groups/0/strategy", name));
        ASSERT_TRUE(r.ok()) << name;
        EXPECT_EQ(r.snapshot->groups[0].strategy, strategy) << name;
    }
}

TEST(ConfigLoader, ListenPortZeroIsAllowedForEphemeralBinding) {
    EXPECT_TRUE(load(with(good_config(), "/listen/port", 0)).ok());
}

TEST(ConfigLoader, SameEndpointInDifferentGroupsIsAllowed) {
    auto j = with(good_config(), "/groups/1/backends/0/address", "127.0.0.1");
    j = with(j, "/groups/1/backends/0/port", 9001);
    const auto r = load(j);
    EXPECT_TRUE(r.ok()) << describe(r);
}

TEST(ConfigLoader, Utf8BomIsAccepted) {
    const auto r = lb::parse_config("\xEF\xBB\xBF" + good_config().dump());
    EXPECT_TRUE(r.ok()) << describe(r);
}

TEST(ConfigLoader, EveryShippedConfigIsValid) {
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(LB_SOURCE_DIR "/config")) {
        if (entry.path().extension() != ".json") continue;
        SCOPED_TRACE(entry.path().filename().string());
        const auto r = lb::load_config_file(entry.path());
        EXPECT_TRUE(r.ok()) << describe(r);
        ++checked;
    }
    EXPECT_GE(checked, 4);  // example, bench, killtest, soak
}

// ---- Rejects each class of bad config (plan IX "Unit": config validation) ------------

TEST(ConfigLoader, EveryFieldIsRequired) {
    const json base = good_config();
    std::vector<json::json_pointer> members;
    collect_members(base, json::json_pointer(), members);
    ASSERT_GT(members.size(), 20u);

    for (const auto& p : members) {
        SCOPED_TRACE(p.to_string());
        json j = base;
        j[p.parent_pointer()].erase(p.back());
        EXPECT_REJECTED_WITH(load(j), p.to_string(), "missing required field");
    }
}

TEST(ConfigLoader, UnknownFieldsAreRejectedAtEveryLevel) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/extra", 1)), "/extra", "unknown field");
    EXPECT_REJECTED_WITH(load(with(good_config(), "/listen/reuse_port", 1)), "/listen/reuse_port", "unknown field");
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/backends/0/note", "x")),
                         "/groups/1/backends/0/note", "unknown field");
}

TEST(ConfigLoader, ErrorPathsEscapeJsonPointerCharacters) {
    EXPECT_REJECTED_WITH(lb::parse_config(R"({"a/b~c": 1})"), "/a~1b~0c", "unknown field");
}

struct BadValueCase {
    const char* pointer;
    json value;
    const char* message_fragment;
};

void PrintTo(const BadValueCase& c, std::ostream* os) {
    *os << c.pointer << " = " << c.value.dump(-1, ' ', /*ensure_ascii=*/true);
}

// Readable ctest names: "<pointer>_<index>", e.g. listen_port_2.
std::string bad_value_name(const ::testing::TestParamInfo<BadValueCase>& info) {
    std::string name;
    for (const char* p = info.param.pointer; *p != '\0'; ++p) {
        const bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (alnum) name += *p;
        else if (!name.empty() && name.back() != '_') name += '_';
    }
    return name + "_" + std::to_string(info.index);
}

class ConfigLoaderBadValue : public ::testing::TestWithParam<BadValueCase> {};

TEST_P(ConfigLoaderBadValue, IsRejectedAtItsPath) {
    const auto& c = GetParam();
    EXPECT_REJECTED_WITH(load(with(good_config(), c.pointer, c.value)), c.pointer, c.message_fragment);
}

INSTANTIATE_TEST_SUITE_P(WrongType, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/listen", "x", "expected an object"},
    BadValueCase{"/listen/port", "8080", "expected an integer"},
    BadValueCase{"/listen/port", 80.5, "expected an integer"},
    BadValueCase{"/listen/port", true, "expected an integer"},
    BadValueCase{"/listen/port", nullptr, "expected an integer"},
    BadValueCase{"/listen/address", 127, "expected a string"},
    BadValueCase{"/workers/threads", 2.0, "expected an integer"},
    BadValueCase{"/workers/threads", "AUTO", "must be \"auto\" or an integer"},
    BadValueCase{"/groups", json::object(), "expected an array"},
    BadValueCase{"/groups/0", json::array(), "expected an object"},
    BadValueCase{"/groups/0/backends", "web-1", "expected an array"},
    BadValueCase{"/groups/0/backends/1", 5, "expected an object"},
    BadValueCase{"/groups/0/backends/0/weight", "3", "expected an integer"},
    BadValueCase{"/routing/default_group", 5, "expected a string"},
    BadValueCase{"/groups/0/sticky", "on", "expected an object"},
    BadValueCase{"/groups/0/passive_health", true, "expected an object"},
    BadValueCase{"/dashboard/graph_points", 9, "between 10 and 3600"},
    BadValueCase{"/groups/0/backends/0/drain", "yes", "must be \"keep\", \"start\" or \"cancel\""},
    BadValueCase{"/groups/0/backends/0/drain", false, "expected a string"},
    BadValueCase{"/timeouts/drain_ms", 0, "between 1 and"},
    BadValueCase{"/groups/0/passive_health/enabled", "yes", "expected true or false"},
    BadValueCase{"/groups/0/passive_health/consecutive_failures", 0, "between 1 and 10000"},
    BadValueCase{"/groups/0/passive_health/count_5xx", 1, "expected true or false"},
    BadValueCase{"/groups/0/sticky/mode", "source_ip", "must be \"off\", \"application_cookie\" or \"inserted_cookie\""},
    BadValueCase{"/groups/0/sticky/cookie", nullptr, "cookie name"},
    BadValueCase{"/groups/0/sticky/cookie", "lb session", "cookie name"},
    BadValueCase{"/groups/1/sticky/cookie", "SESSIONID", "must be null when mode is \"off\""},
    BadValueCase{"/groups/0/sticky/ttl_ms", 999, "between 1000 and 604800000"},
    BadValueCase{"/sticky_table/shards", 0, "between 1 and 1024"},
    BadValueCase{"/sticky_table/max_entries", 0, "between 1 and 100000000"},
    BadValueCase{"/sticky_table/max_entries", 15, "at least the number of shards"},
    BadValueCase{"/routing/rules", json::object(), "expected an array"},
    BadValueCase{"/routing/rules/0", "x", "expected an object"},
    BadValueCase{"/routing/rules/0/type", "regex", "must be \"path_prefix\", \"path_glob\", \"header\" or \"cookie\""},
    BadValueCase{"/routing/rules/0/field", "X-Path", "must be null for path rules"},
    BadValueCase{"/routing/rules/0/value", nullptr, "a path rule needs a path"},
    BadValueCase{"/routing/rules/0/value", "api", "must start with '/'"},
    BadValueCase{"/routing/rules/0/value", "/api?x=1", "without '?' or '#'"},
    BadValueCase{"/routing/rules/0/value", 7, "expected a string or null"},
    BadValueCase{"/routing/rules/1/value", "/static/a b.css", "must start with '/'"},
    BadValueCase{"/routing/rules/2/field", nullptr, "header name"},
    BadValueCase{"/routing/rules/2/field", "X Beta", "header name"},
    BadValueCase{"/routing/rules/2/value", " on", "without leading or trailing spaces"},
    BadValueCase{"/routing/rules/3/field", "can;ary", "cookie name"},
    BadValueCase{"/routing/rules/3/value", "a b", "visible ASCII"},
    BadValueCase{"/routing/rules/0/group", "nope", "unknown group \"nope\""},
    BadValueCase{"/routing/rules/1/id", "api-path", "duplicate rule id \"api-path\""},
    BadValueCase{"/routing/rules/0/id", "has space", "characters of [A-Za-z0-9._-]"}), bad_value_name);

INSTANTIATE_TEST_SUITE_P(OutOfRange, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/listen/port", 65536, "between 0 and 65535"},
    BadValueCase{"/listen/port", -1, "between 0 and 65535"},
    BadValueCase{"/listen/port", json::parse("18446744073709551616"), "expected an integer"},
    BadValueCase{"/groups/0/backends/0/port", 0, "between 1 and 65535"},
    BadValueCase{"/workers/threads", 0, "between 1 and 256"},
    BadValueCase{"/workers/threads", 257, "between 1 and 256"},
    BadValueCase{"/groups/0/backends/0/weight", 0, "between 1 and 1000"},
    BadValueCase{"/groups/0/backends/0/weight", 1001, "between 1 and 1000"},
    BadValueCase{"/limits/max_request_line_bytes", 63, "between 64 and 1048576"},
    BadValueCase{"/limits/max_request_header_bytes", 1048577, "between 256 and 1048576"},
    BadValueCase{"/limits/max_request_header_count", 0, "between 1 and 10000"},
    BadValueCase{"/limits/max_response_header_bytes", 255, "between 256 and 1048576"},
    BadValueCase{"/limits/max_response_header_count", 10001, "between 1 and 10000"},
    BadValueCase{"/limits/max_chunk_line_bytes", 15, "between 16 and 65536"},
    BadValueCase{"/limits/max_client_connections", 0, "between 1 and 1000000"},
    BadValueCase{"/listen/backlog", 0, "between 1 and 65535"},
    BadValueCase{"/listen/pending_accepts", 1025, "between 1 and 1024"},
    BadValueCase{"/buffers/client_read_bytes", 1023, "between 1024 and 1048576"},
    BadValueCase{"/buffers/backend_read_bytes", 1048577, "between 1024 and 1048576"},
    BadValueCase{"/timeouts/shutdown_grace_ms", 600001, "between 0 and 600000"},
    BadValueCase{"/pool/max_connections_per_backend", 0, "between 1 and 65535"},
    BadValueCase{"/pool/max_idle_per_backend", 129, "must not exceed max_connections_per_backend"},
    BadValueCase{"/pool/idle_timeout_ms", 0, "between 1 and 3600000"},
    BadValueCase{"/pool/max_waiters_per_backend", 1000001, "between 0 and 1000000"},
    BadValueCase{"/pool/wait_timeout_ms", 0, "between 1 and 600000"},
    BadValueCase{"/pool/fail_fast_connect", "true", "expected true or false"},
    BadValueCase{"/maintenance/interval_ms", 9, "between 10 and 60000"},
    BadValueCase{"/metrics/slice_ms", 99, "between 100 and 60000"},
    BadValueCase{"/metrics/max_backend_series", 2, "smaller than the number of backends (3)"},
    BadValueCase{"/metrics/max_backend_series", 4097, "between 1 and 4096"},
    BadValueCase{"/config_reload/watch_file", 1, "expected true or false"},
    BadValueCase{"/config_reload/debounce_ms", 9, "between 10 and 60000"},
    BadValueCase{"/balancing/response_time_decay_ms", 99, "between 100 and 600000"},
    BadValueCase{"/balancing/response_time_expiry_ms", 3600001, "between 100 and 3600000"},
    BadValueCase{"/dashboard/publish_interval_ms", 49, "between 50 and 10000"},
    BadValueCase{"/dashboard/event_rows", 9, "between 10 and 100000"},
    BadValueCase{"/metrics/window_slices", 0, "between 1 and 120"},
    BadValueCase{"/event_log/path", 5, "expected a string"},
    BadValueCase{"/event_log/max_file_bytes", 100, "between 4096 and 1073741824"},
    BadValueCase{"/event_log/max_files", 0, "between 1 and 100"},
    BadValueCase{"/event_log/max_queue", 99, "between 100 and 10000000"},
    BadValueCase{"/event_log/recent_events", 100001, "between 0 and 100000"},
    BadValueCase{"/event_log/trace_requests", "yes", "expected true or false"},
    BadValueCase{"/event_log/trace_requests", 1, "expected true or false"},
    BadValueCase{"/timeouts/client_header_ms", 0, "between 1 and 600000"},
    BadValueCase{"/timeouts/backend_response_ms", 600001, "between 1 and 600000"},
    BadValueCase{"/groups/0/health/type", "icmp", "must be \"http\" or \"tcp\""},
    BadValueCase{"/groups/0/health/path", "health", "must start with '/'"},
    BadValueCase{"/groups/0/health/path", "/a b", "must start with '/'"},
    BadValueCase{"/groups/0/health/interval_ms", 9, "between 10 and 3600000"},
    BadValueCase{"/groups/0/health/timeout_ms", 2001, "must not exceed interval_ms"},
    BadValueCase{"/groups/0/health/unhealthy_threshold", 0, "between 1 and 100"},
    BadValueCase{"/groups/0/health/healthy_threshold", 101, "between 1 and 100"},
    BadValueCase{"/groups/0/strategy", "random", "must be \"round_robin\", \"least_connections\", \"weighted_round_robin\", \"least_response_time\" or \"ip_hash\""},
    BadValueCase{"/groups/0/strategy", 2, "expected a string"},
    BadValueCase{"/groups/0/host_header", "rewrite", "must be \"preserve\" or \"backend\""},
    BadValueCase{"/groups/0/host_header", 1, "expected a string"},
    BadValueCase{"/trusted_proxies", "10.0.0.0/8", "expected an array"},
    BadValueCase{"/trusted_proxies/0", "10.0.0.0/33", "IPv4 address or CIDR"},
    BadValueCase{"/trusted_proxies/0", "10.0.0.1/8", "without host bits"},
    BadValueCase{"/trusted_proxies/0", "localhost", "IPv4 address or CIDR"},
    BadValueCase{"/trusted_proxies/0", "10.0.0.0/", "IPv4 address or CIDR"},
    BadValueCase{"/trusted_proxies/1", 7, "IPv4 address or CIDR"}), bad_value_name);

TEST(ConfigLoader, EmptyTrustedProxiesListIsValid) {
    const auto r = load(with(good_config(), "/trusted_proxies", json::array()));
    ASSERT_TRUE(r.ok()) << describe(r);
    EXPECT_TRUE(r.snapshot->trusted_proxies.empty());
    EXPECT_FALSE(r.snapshot->is_trusted_proxy(0x7F000001));
}

INSTANTIATE_TEST_SUITE_P(BadAddress, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/listen/address", "localhost", "IPv4 address literal"},
    BadValueCase{"/listen/address", "", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "1.2.3", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "256.1.1.1", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "::1", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", " 127.0.0.1", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "0.0.0.0", "not a connectable backend address"}), bad_value_name);

INSTANTIATE_TEST_SUITE_P(BadName, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/groups/0/name", "", "1-64 characters"},
    BadValueCase{"/groups/0/name", "has space", "1-64 characters"},
    BadValueCase{"/groups/0/name", std::string(65, 'a'), "1-64 characters"},
    BadValueCase{"/groups/0/backends/0/id", "web/1", "1-64 characters"},
    BadValueCase{"/groups/0/backends/0/id", "w\xC3\xA9" "b", "1-64 characters"}), bad_value_name);

TEST(ConfigLoader, EmptyGroupListIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups", json::array())), "/groups", "at least one group");
}

TEST(ConfigLoader, GroupWithoutBackendsIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/backends", json::array())), "/groups/1/backends",
                         "at least one backend");
}

TEST(ConfigLoader, DuplicateGroupNameIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/name", "web")), "/groups/1/name",
                         "duplicate group name (first at /groups/0/name)");
}

TEST(ConfigLoader, DuplicateBackendIdAcrossGroupsIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/backends/0/id", "web-1")), "/groups/1/backends/0/id",
                         "duplicate backend id (first at /groups/0/backends/0/id)");
}

TEST(ConfigLoader, DuplicateEndpointWithinGroupIsRejected) {
    auto j = with(good_config(), "/groups/0/backends/1/address", "127.0.0.1");
    j = with(j, "/groups/0/backends/1/port", 9001);
    EXPECT_REJECTED_WITH(load(j), "/groups/0/backends/1", "duplicate backend address:port");
}

TEST(ConfigLoader, UnknownDefaultGroupIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/routing/default_group", "nope")), "/routing/default_group",
                         "unknown group \"nope\"");
}

TEST(ConfigLoader, DuplicateJsonKeysAreRejected) {
    std::string text = good_config().dump();
    const auto replace = [&text](std::string_view from, std::string_view to) {
        const auto pos = text.find(from);
        ASSERT_NE(pos, std::string::npos) << from;
        text.replace(pos, from.size(), to);
    };
    replace(R"("port":8080)", R"("port":8080,"port":8081)");
    replace(R"("id":"api-1")", R"("id":"api-1","id":"api-9")");

    const auto r = lb::parse_config(text);
    EXPECT_REJECTED_WITH(r, "/listen/port", "duplicate key");
    EXPECT_REJECTED_WITH(r, "/groups/1/backends/0/id", "duplicate key");
}

TEST(ConfigLoader, MalformedJsonIsRejectedWithPosition) {
    const auto r = lb::parse_config("{\n  \"listen\": }");
    EXPECT_REJECTED_WITH(r, "", "invalid JSON");
    EXPECT_REJECTED_WITH(r, "", "line 2");
}

TEST(ConfigLoader, NonObjectDocumentsAreRejected) {
    EXPECT_REJECTED_WITH(lb::parse_config(""), "", "invalid JSON");
    EXPECT_REJECTED_WITH(lb::parse_config("[]"), "", "expected an object");
    EXPECT_REJECTED_WITH(lb::parse_config("null"), "", "expected an object");
}

TEST(ConfigLoader, CommentsAreNotJson) {
    EXPECT_REJECTED_WITH(lb::parse_config("// note\n" + good_config().dump()), "", "invalid JSON");
}

TEST(ConfigLoader, HalfWrittenFileIsRejected) {
    const std::string full = good_config().dump(2);
    EXPECT_REJECTED_WITH(lb::parse_config(full.substr(0, full.size() / 2)), "", "invalid JSON");
}

TEST(ConfigLoader, ReportsAllErrorsNotJustTheFirst) {
    auto j = with(good_config(), "/listen/port", "x");
    j = with(j, "/groups/0/backends/1/weight", 0);
    j = with(j, "/workers/extra", true);
    const auto r = load(j);
    EXPECT_REJECTED_WITH(r, "/listen/port", "expected an integer");
    EXPECT_REJECTED_WITH(r, "/groups/0/backends/1/weight", "between 1 and 1000");
    EXPECT_REJECTED_WITH(r, "/workers/extra", "unknown field");
}

TEST(ConfigLoader, MissingFileIsRejected) {
    EXPECT_REJECTED_WITH(lb::load_config_file(LB_SOURCE_DIR "/config/does-not-exist.json"), "",
                         "cannot open config file");
}

TEST(ConfigLoader, ErrorToStringShowsRootAndPath) {
    EXPECT_EQ(lb::to_string({"", "bad"}), "(root): bad");
    EXPECT_EQ(lb::to_string({"/listen/port", "bad"}), "/listen/port: bad");
}
