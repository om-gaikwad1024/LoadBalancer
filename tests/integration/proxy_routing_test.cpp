// Step 2.3 (plan IV.8): content-aware routing through the running proxy. Routing picks the
// group for every request, before affinity and load balancing (plan III).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;

namespace {

nlohmann::json backend_json(const std::string& id, std::uint16_t port) {
    return {{"id", id}, {"address", "127.0.0.1"}, {"port", port}, {"weight", 1}};
}

nlohmann::json rule(const std::string& id, const std::string& type, const nlohmann::json& field,
                    const nlohmann::json& value, const std::string& group) {
    return {{"id", id}, {"type", type}, {"field", field}, {"value", value}, {"group", group}};
}

// Group "web" has the backends the fixture was given; this adds group "api" (and the rules).
void add_api_group(nlohmann::json& j, const std::vector<nlohmann::json>& backends, nlohmann::json rules) {
    auto api = j["groups"][0];
    api["name"] = "api";
    api["backends"] = backends;
    j["groups"].push_back(api);
    j["routing"]["rules"] = std::move(rules);
}

nlohmann::json example_rules() {
    return nlohmann::json::array({
        rule("api-path", "path_prefix", nullptr, "/api", "api"),
        rule("api-header", "header", "X-Group", "api", "api"),
        rule("beta-cookie", "cookie", "beta", nullptr, "api"),
        rule("reports", "path_glob", nullptr, "/reports/*.csv", "api"),
    });
}

std::string served_by(std::uint16_t port, std::string_view target, std::string_view headers = {}) {
    const auto r = fetch(port, target, headers);
    return r.status == 200 ? r.header("x-backend-id") : "status " + std::to_string(r.status);
}

}  // namespace

// Plan IV.8 done: two requests that differ only in path, header or cookie land in
// different groups.
TEST_F(ProxyTest, RequestsDifferingOnlyInPathHeaderOrCookieLandInDifferentGroups) {
    const auto web = start_backend({}, "web-1");
    const auto api = start_backend({}, "api-1");
    start_proxy({web}, [&](nlohmann::json& j) { add_api_group(j, {backend_json("api-1", api)}, example_rules()); });

    EXPECT_EQ(served_by(proxy_port(), "/home"), "web-1");
    EXPECT_EQ(served_by(proxy_port(), "/api/users"), "api-1");  // path
    EXPECT_EQ(served_by(proxy_port(), "/apiary"), "web-1");     // not the /api segment
    EXPECT_EQ(served_by(proxy_port(), "/reports/2026/q1.csv"), "api-1");
    EXPECT_EQ(served_by(proxy_port(), "/reports/q1.pdf"), "web-1");

    EXPECT_EQ(served_by(proxy_port(), "/home", "X-Group: api\r\n"), "api-1");  // header
    EXPECT_EQ(served_by(proxy_port(), "/home", "X-Group: web\r\n"), "web-1");

    EXPECT_EQ(served_by(proxy_port(), "/home", "Cookie: theme=dark; beta=1\r\n"), "api-1");  // cookie
    EXPECT_EQ(served_by(proxy_port(), "/home", "Cookie: theme=dark\r\n"), "web-1");

    // Every request on one keep-alive connection is routed on its own.
    TestClient c(proxy_port());
    for (const char* target : {"/a", "/api/b", "/c", "/api"}) {
        ASSERT_TRUE(c.send(get_request(target)));
        const auto r = c.read_response();
        EXPECT_EQ(r.header("x-backend-id"), std::string(target).rfind("/api", 0) == 0 ? "api-1" : "web-1") << target;
    }
}

// Plan IV.8 done: a client stuck to a backend in group A that requests a path belonging to
// group B reaches group B. Affinity here is IP hash (the client identity is the
// X-Forwarded-For address from a trusted proxy); cookie stickiness arrives in step 2.4.
TEST_F(ProxyTest, ClientStuckToABackendInOneGroupStillReachesTheGroupItsPathRoutesTo) {
    const auto w1 = start_backend({}, "web-1");
    const auto w2 = start_backend({}, "web-2");
    const auto w3 = start_backend({}, "web-3");
    const auto api = start_backend({}, "api-1");
    start_proxy({w1, w2, w3}, [&](nlohmann::json& j) {
        j["groups"][0]["strategy"] = "ip_hash";
        j["trusted_proxies"] = nlohmann::json::array({"127.0.0.1/32"});
        add_api_group(j, {backend_json("api-1", api)}, example_rules());
    });
    for (int client = 0; client < 12; ++client) {
        const std::string xff = "X-Forwarded-For: 203.0.113." + std::to_string(client) + "\r\n";
        const std::string home = served_by(proxy_port(), "/home", xff);
        ASSERT_EQ(home.rfind("web-", 0), 0u) << home;
        EXPECT_EQ(served_by(proxy_port(), "/home/again", xff), home);  // stuck in group web
        EXPECT_EQ(served_by(proxy_port(), "/api/orders", xff), "api-1") << "client " << client;
        EXPECT_EQ(served_by(proxy_port(), "/home", xff), home);  // and still stuck afterwards
    }
}

// Routing never falls back to another group: a routed group with no eligible backend is a 503,
// logged with the group and the rule that chose it.
TEST_F(ProxyTest, RoutedGroupWithoutEligibleBackendIs503NotTheDefaultGroup) {
    const auto web = start_backend({}, "web-1");
    const auto api = start_backend({}, "api-1");
    start_proxy({web}, [&](nlohmann::json& j) { add_api_group(j, {backend_json("api-1", api)}, example_rules()); });
    ASSERT_TRUE(engine().set_backend_state("api-1", lb::BackendState::Unhealthy));
    const auto r = fetch(proxy_port(), "/api/x");
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(backend(0).stats().requests, 0u);
    EXPECT_EQ(served_by(proxy_port(), "/home"), "web-1");
    bool logged = false;
    for (const auto& e : engine().recent_events()) {
        if (e.type == "no_backend_available" && e.request_id == r.header("x-request-id")) {
            logged = e.json.find("\"group\":\"api\"") != std::string::npos &&
                     e.json.find("\"rule\":\"api-path\"") != std::string::npos;
        }
    }
    EXPECT_TRUE(logged);
}

// Plan IV.18: the routing decision is a step of its own, with the group and rule in debug mode.
TEST_F(ProxyTest, DebugTraceNamesTheRoutedGroupAndRule) {
    const auto web = start_backend({}, "web-1");
    const auto api = start_backend({}, "api-1");
    start_proxy({web}, [&](nlohmann::json& j) {
        j["event_log"]["trace_requests"] = true;
        add_api_group(j, {backend_json("api-1", api)}, example_rules());
    });
    const auto routed = fetch(proxy_port(), "/api/x");
    const auto fallback = fetch(proxy_port(), "/x");
    const auto message_of = [&](const std::string& request_id) {
        for (const auto& e : engine().recent_events()) {
            if (e.request_id == request_id && e.json.find("\"step\":\"group_routed\"") != std::string::npos) {
                return e.message;
            }
        }
        return std::string();
    };
    EXPECT_TRUE(eventually([&] { return !message_of(fallback.header("x-request-id")).empty(); }));
    EXPECT_EQ(message_of(routed.header("x-request-id")), "group_routed: api (rule api-path)");
    EXPECT_EQ(message_of(fallback.header("x-request-id")), "group_routed: web (default)");
}

// Plan IV.14 with IV.8: routing rules change under load with no failed request.
TEST_F(ProxyTest, RoutingRulesChangeUnderLoadWithoutErrors) {
    const auto web = start_backend({}, "web-1");
    const auto api = start_backend({}, "api-1");
    const auto config = [&](nlohmann::json rules) {
        return lbtest::make_proxy_config({web}, [&](nlohmann::json& j) {
            add_api_group(j, {backend_json("api-1", api)}, rules);
        });
    };
    start_proxy({web}, [&](nlohmann::json& j) { add_api_group(j, {backend_json("api-1", api)}, nlohmann::json::array()); });

    std::atomic<bool> stop{false};
    std::atomic<int> ok{0};
    std::atomic<int> failed{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t) {
        clients.emplace_back([&] {
            TestClient c(proxy_port());
            while (!stop) {
                if (!c.send(get_request("/api/load"))) {
                    ++failed;
                    return;
                }
                const auto r = c.read_response();
                (r.status == 200 && r.end == lbtest::ClientResponse::End::Complete ? ok : failed)++;
            }
        });
    }
    ASSERT_TRUE(eventually([&] { return backend(0).stats().requests > 200; }));
    EXPECT_EQ(backend(1).stats().requests, 0u);

    // Add the /api rule: new requests switch to group api.
    ASSERT_TRUE(engine().reload(config(nlohmann::json::array({rule("api-path", "path_prefix", nullptr, "/api", "api")})))
                    .accepted);
    ASSERT_TRUE(eventually([&] { return backend(1).stats().requests > 200; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // requests that started before the swap finish
    const auto web_after = backend(0).stats().requests;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(backend(0).stats().requests, web_after);

    // Remove it again: back to the default group.
    ASSERT_TRUE(engine().reload(config(nlohmann::json::array())).accepted);
    const auto web_before = backend(0).stats().requests;
    ASSERT_TRUE(eventually([&] { return backend(0).stats().requests > web_before + 200; }));
    stop = true;
    for (auto& t : clients) t.join();
    EXPECT_EQ(failed.load(), 0);
    EXPECT_GT(ok.load(), 600);
}
