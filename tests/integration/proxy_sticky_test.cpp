// Step 2.4 (plan IV.9): sticky sessions per group, checked after routing (plan III).

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "scripted_backend.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::ProxyTest;
using lbtest::ScriptedBackend;

namespace {

void sticky(nlohmann::json& group, const char* mode, const nlohmann::json& cookie, int ttl_ms = 600000) {
    group["sticky"] = {{"mode", mode}, {"cookie", cookie}, {"ttl_ms", ttl_ms}};
}

// The cookie value from a response's Set-Cookie header (no attributes), or empty.
std::string set_cookie_value(const ClientResponse& r, const std::string& name) {
    const std::string h = r.header("set-cookie");
    if (h.rfind(name + "=", 0) != 0) return {};
    return h.substr(name.size() + 1, h.find(';') - name.size() - 1);
}

std::string with_cookie(const std::string& name, const std::string& value) {
    return "Cookie: theme=dark; " + name + "=" + value + "\r\n";
}

std::size_t count_events(lb::Engine& engine, const std::string& type) {
    std::size_t n = 0;
    for (const auto& e : engine.recent_events()) n += e.type == type ? 1 : 0;
    return n;
}

}  // namespace

// Plan IV.9 done (first half): a client sending the same cookie reaches the same backend
// across repeated requests. With inserted_cookie the proxy issues the cookie itself.
TEST_F(ProxyTest, InsertedCookieKeepsAClientOnOneBackend) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) { sticky(j["groups"][0], "inserted_cookie", "lb_session"); });

    std::map<std::string, int> clients_per_backend;
    for (int client = 0; client < 12; ++client) {
        const auto first = fetch(proxy_port(), "/");
        ASSERT_EQ(first.status, 200);
        const std::string key = set_cookie_value(first, "lb_session");
        ASSERT_EQ(key.size(), 32u) << first.header("set-cookie");
        EXPECT_NE(first.header("set-cookie").find("; Path=/; HttpOnly; SameSite=Lax"), std::string::npos);
        const std::string backend = first.header("x-backend-id");
        clients_per_backend[backend]++;
        for (int i = 0; i < 10; ++i) {  // new connections each time: only the cookie ties them
            const auto r = fetch(proxy_port(), "/again", with_cookie("lb_session", key));
            EXPECT_EQ(r.header("x-backend-id"), backend) << "client " << client << " request " << i;
            EXPECT_TRUE(r.header("set-cookie").empty());  // already has one
        }
    }
    // New sessions are still spread by the group's balancer (round robin).
    EXPECT_EQ(clients_per_backend.size(), 3u);
    const auto s = engine().stats();
    EXPECT_EQ(s.sticky_assignments, 12u);
    EXPECT_EQ(s.sticky_hits, 120u);
    EXPECT_EQ(s.sticky_entries, 12u);
}

// Plan IV.9 done (second half): after the sticky backend is killed, the next request goes to
// a new backend in the same group and later requests stay there.
TEST_F(ProxyTest, StickyClientMovesToANewBackendWhenItsBackendDiesAndStaysThere) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) {
        sticky(j["groups"][0], "inserted_cookie", "lb_session");
        auto& h = j["groups"][0]["health"];
        h["interval_ms"] = 50;
        h["timeout_ms"] = 40;
        h["unhealthy_threshold"] = 2;
        h["healthy_threshold"] = 2;
    });
    const auto first = fetch(proxy_port(), "/");
    const std::string key = set_cookie_value(first, "lb_session");
    const std::string original = first.header("x-backend-id");
    ASSERT_FALSE(key.empty());
    const std::size_t index = original == "b1" ? 0 : original == "b2" ? 1 : 2;

    backend(index).stop();  // killed
    ASSERT_TRUE(eventually([&] {
        for (const auto& b : engine().backend_stats()) {
            if (b.id == original) return b.state == BackendState::Unhealthy;
        }
        return false;
    }));

    const auto moved = fetch(proxy_port(), "/after-kill", with_cookie("lb_session", key));
    ASSERT_EQ(moved.status, 200);
    const std::string replacement = moved.header("x-backend-id");
    EXPECT_NE(replacement, original);
    EXPECT_TRUE(moved.header("set-cookie").empty());  // same session, new backend
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(fetch(proxy_port(), "/later", with_cookie("lb_session", key)).header("x-backend-id"), replacement);
    }
    EXPECT_EQ(engine().stats().sticky_reassignments, 1u);
    bool logged = false;
    for (const auto& e : engine().recent_events()) {
        if (e.type == "sticky_reassigned" && e.request_id == moved.header("x-request-id")) {
            logged = e.message == "session in group web moved from " + original + " (unhealthy) to " + replacement;
        }
    }
    EXPECT_TRUE(logged);

    // The old backend coming back does not pull the session back: it stays where it moved.
    start_backend({}, original, original == "b1" ? p1 : original == "b2" ? p2 : p3);
    ASSERT_TRUE(eventually([&] {
        for (const auto& b : engine().backend_stats()) {
            if (b.id == original) return b.state == BackendState::Healthy;
        }
        return false;
    }));
    EXPECT_EQ(fetch(proxy_port(), "/", with_cookie("lb_session", key)).header("x-backend-id"), replacement);
}

// Plan IV.12: a draining backend gets no new requests and no new sticky assignments.
TEST_F(ProxyTest, DrainingStickyBackendHandsItsSessionsToAnotherBackend) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) { sticky(j["groups"][0], "inserted_cookie", "lb_session"); });
    const auto first = fetch(proxy_port(), "/");
    const std::string key = set_cookie_value(first, "lb_session");
    const std::string original = first.header("x-backend-id");
    ASSERT_TRUE(engine().set_backend_state(original, BackendState::Draining));
    const auto moved = fetch(proxy_port(), "/", with_cookie("lb_session", key));
    EXPECT_NE(moved.header("x-backend-id"), original);
    for (const auto& e : engine().recent_events()) {
        if (e.type == "sticky_reassigned") EXPECT_NE(e.message.find("(draining)"), std::string::npos) << e.message;
    }
    // New sessions avoid it too.
    for (int i = 0; i < 6; ++i) EXPECT_NE(fetch(proxy_port(), "/").header("x-backend-id"), original);
}

// Application cookie mode: the backend's own session cookie, learned from its Set-Cookie,
// keeps the client on the backend that created the session.
TEST_F(ProxyTest, ApplicationCookieIsLearnedFromTheBackendsSetCookie) {
    const auto script = [](const char* id) {
        return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Backend-Id: ") + id +
               "\r\nSet-Cookie: SESSIONID=session-of-" + id + "; Path=/; HttpOnly\r\n\r\nok";
    };
    ScriptedBackend a(script("A"));
    ScriptedBackend b(script("B"));
    start_proxy({a.port(), b.port()}, [](nlohmann::json& j) { sticky(j["groups"][0], "application_cookie", "SESSIONID"); });

    const auto first = fetch(proxy_port(), "/login");
    const std::string backend = first.header("x-backend-id");
    const std::string session = set_cookie_value(first, "SESSIONID");
    ASSERT_EQ(session, "session-of-" + backend);  // the backend's cookie passes through unchanged
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(fetch(proxy_port(), "/cart", with_cookie("SESSIONID", session)).header("x-backend-id"), backend);
    }
    // Without the cookie, round robin carries on.
    std::set<std::string> seen;
    for (int i = 0; i < 4; ++i) seen.insert(fetch(proxy_port(), "/").header("x-backend-id"));
    EXPECT_EQ(seen.size(), 2u);
}

// A cookie value the table has never seen (expired, or issued before a restart) becomes a
// session on whichever backend the balancer picks, and sticks from then on.
TEST_F(ProxyTest, UnknownSessionCookieIsMappedOnFirstUse) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) { sticky(j["groups"][0], "application_cookie", "SESSIONID"); });
    const std::string cookie = with_cookie("SESSIONID", "issued-before-a-restart");
    const std::string first = fetch(proxy_port(), "/", cookie).header("x-backend-id");
    for (int i = 0; i < 8; ++i) EXPECT_EQ(fetch(proxy_port(), "/", cookie).header("x-backend-id"), first);
}

// A forged or malformed proxy cookie is not trusted as a key: the client gets a new one.
TEST_F(ProxyTest, MalformedInsertedCookieIsReplaced) {
    const auto p1 = start_backend({}, "b1");
    start_proxy({p1}, [](nlohmann::json& j) { sticky(j["groups"][0], "inserted_cookie", "lb_session"); });
    const auto r = fetch(proxy_port(), "/", with_cookie("lb_session", "../../etc/passwd"));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(set_cookie_value(r, "lb_session").size(), 32u);
}

// Plan IV.8 done, with cookie stickiness: a client stuck to a backend in group A that
// requests a path belonging to group B reaches group B, and is still stuck in A afterwards.
TEST_F(ProxyTest, StickyClientStillReachesTheGroupItsPathRoutesTo) {
    const auto w1 = start_backend({}, "web-1");
    const auto w2 = start_backend({}, "web-2");
    const auto a1 = start_backend({}, "api-1");
    start_proxy({w1, w2}, [&](nlohmann::json& j) {
        auto& web = j["groups"][0];
        web["backends"][0]["id"] = "web-1";
        web["backends"][1]["id"] = "web-2";
        sticky(web, "inserted_cookie", "lb_session");
        auto api = web;
        api["name"] = "api";
        api["backends"] = nlohmann::json::array({{{"id", "api-1"}, {"address", "127.0.0.1"}, {"port", a1}, {"weight", 1}}});
        sticky(api, "off", nullptr);
        j["groups"].push_back(api);
        j["routing"]["rules"] = nlohmann::json::array(
            {{{"id", "api"}, {"type", "path_prefix"}, {"field", nullptr}, {"value", "/api"}, {"group", "api"}}});
    });
    for (int client = 0; client < 6; ++client) {
        const auto first = fetch(proxy_port(), "/home");
        const std::string key = set_cookie_value(first, "lb_session");
        const std::string home = first.header("x-backend-id");
        ASSERT_FALSE(key.empty());
        EXPECT_EQ(fetch(proxy_port(), "/api/orders", with_cookie("lb_session", key)).header("x-backend-id"), "api-1");
        EXPECT_EQ(fetch(proxy_port(), "/home", with_cookie("lb_session", key)).header("x-backend-id"), home);
    }
}

// Sessions survive a hot reload: the table is the engine's, not the config's.
TEST_F(ProxyTest, StickySessionsSurviveAReload) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto tweak = [](int weight) {
        return [weight](nlohmann::json& j) {
            sticky(j["groups"][0], "inserted_cookie", "lb_session");
            j["groups"][0]["backends"][1]["weight"] = weight;
        };
    };
    start_proxy({p1, p2}, tweak(1));
    std::vector<std::pair<std::string, std::string>> sessions;  // key, backend
    for (int i = 0; i < 6; ++i) {
        const auto r = fetch(proxy_port(), "/");
        sessions.emplace_back(set_cookie_value(r, "lb_session"), r.header("x-backend-id"));
    }
    ASSERT_TRUE(engine().reload(lbtest::make_proxy_config({p1, p2}, tweak(5))).accepted);
    for (const auto& [key, backend] : sessions) {
        EXPECT_EQ(fetch(proxy_port(), "/", with_cookie("lb_session", key)).header("x-backend-id"), backend);
    }
}

// When the table is full, new sessions are served but not sticky, counted, and reported
// once per maintenance interval.
TEST_F(ProxyTest, FullStickyTableServesNewSessionsWithoutStickiness) {
    const auto p1 = start_backend({}, "b1");
    start_proxy({p1}, [](nlohmann::json& j) {
        sticky(j["groups"][0], "inserted_cookie", "lb_session");
        j["sticky_table"] = {{"shards", 1}, {"max_entries", 2}};
    });
    EXPECT_FALSE(set_cookie_value(fetch(proxy_port(), "/"), "lb_session").empty());
    EXPECT_FALSE(set_cookie_value(fetch(proxy_port(), "/"), "lb_session").empty());
    const auto third = fetch(proxy_port(), "/");
    EXPECT_EQ(third.status, 200);
    EXPECT_TRUE(third.header("set-cookie").empty());  // no cookie for a session that is not stored
    EXPECT_EQ(engine().stats().sticky_not_stored, 1u);
    EXPECT_TRUE(eventually([&] { return count_events(engine(), "sticky_table_full") == 1; }));
}

// Expired mappings are removed by the maintenance thread (plan V).
TEST_F(ProxyTest, ExpiredStickyMappingsAreSwept) {
    const auto p1 = start_backend({}, "b1");
    start_proxy({p1}, [](nlohmann::json& j) { sticky(j["groups"][0], "inserted_cookie", "lb_session", 1000); });
    for (int i = 0; i < 5; ++i) fetch(proxy_port(), "/");
    EXPECT_EQ(engine().stats().sticky_entries, 5u);
    EXPECT_TRUE(eventually([&] { return engine().stats().sticky_entries == 0; }, std::chrono::milliseconds(3000)));
}

// Plan IV.18: the affinity lookup is its own step, after routing and before selection.
TEST_F(ProxyTest, DebugTraceShowsTheAffinityStepInOrder) {
    const auto p1 = start_backend({}, "b1");
    start_proxy({p1}, [](nlohmann::json& j) {
        sticky(j["groups"][0], "inserted_cookie", "lb_session");
        j["event_log"]["trace_requests"] = true;
    });
    const auto first = fetch(proxy_port(), "/");
    const auto second = fetch(proxy_port(), "/", with_cookie("lb_session", set_cookie_value(first, "lb_session")));
    const auto steps_of = [&](const std::string& id) {
        std::vector<std::string> steps;
        for (const auto& e : engine().recent_events()) {
            if (e.type == "request_step" && e.request_id == id) steps.push_back(e.message);
        }
        return steps;
    };
    const std::string id = second.header("x-request-id");
    ASSERT_TRUE(eventually([&] { return steps_of(id).size() == 8; }));
    const auto steps = steps_of(id);
    EXPECT_EQ(steps[0], "request_received");
    EXPECT_EQ(steps[1], "group_routed: web (default)");
    EXPECT_EQ(steps[2], "affinity_checked: sticky to b1");
    EXPECT_EQ(steps[3], "backend_selected");
    EXPECT_EQ(steps_of(first.header("x-request-id"))[2], "affinity_checked: no session cookie; b1");
}
