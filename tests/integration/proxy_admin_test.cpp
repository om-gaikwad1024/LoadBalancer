// Step 2.7 (plan IV.17, IV.14): admin edits go through the config manager's validation path,
// apply like any reload, and are saved to the config file only if accepted.

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const lb::BackendStats* stats_of(const std::vector<lb::BackendStats>& all, const std::string& id) {
    for (const auto& b : all) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

class AdminTest : public ProxyTest {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("lb-admin-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount64()));
        fs::create_directories(dir_);
        path_ = dir_ / "lb.json";
    }
    void TearDown() override {
        if (engine_) engine_->stop();
        ProxyTest::TearDown();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // Writes the config file, starts the engine from it and registers it.
    void start_from_file(const std::vector<std::uint16_t>& ports,
                         const std::function<void(nlohmann::json&)>& tweak = {}) {
        const auto j = lbtest::make_proxy_config_json(ports, [&](nlohmann::json& c) {
            c["config_reload"]["watch_file"] = true;
            c["config_reload"]["debounce_ms"] = 100;
            if (tweak) tweak(c);
        });
        std::ofstream(path_, std::ios::binary) << j.dump(2);
        auto loaded = lb::load_config_file(path_);
        ASSERT_TRUE(loaded.ok());
        engine_ = std::make_unique<lb::Engine>(loaded.snapshot);
        std::string error;
        ASSERT_TRUE(engine_->start(&error)) << error;
        ASSERT_TRUE(engine_->watch_config_file(path_, &error)) << error;
    }

    nlohmann::json saved() { return nlohmann::json::parse(read_text(path_)); }
    lb::Engine& proxy() { return *engine_; }
    std::uint16_t port() { return engine_->listen_port(); }

    fs::path dir_;
    fs::path path_;
    std::unique_ptr<lb::Engine> engine_;
};

lb::BackendEdit backend_edit(const std::string& id, std::uint16_t port, std::uint32_t weight = 1) {
    lb::BackendEdit b;
    b.group = "web";
    b.id = id;
    b.address = "127.0.0.1";
    b.port = port;
    b.weight = weight;
    return b;
}

}  // namespace

// Plan IV.17 done (engine half): a backend added from the admin console takes traffic at once,
// without a restart, and the change is saved; the watcher does not reload it a second time.
TEST_F(AdminTest, AddedBackendTakesTrafficAndIsSavedOnce) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_from_file({p1});
    const auto r = proxy().admin_add_backend(backend_edit("b2", p2));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.summary, "added backend b2 (added b2)");
    ASSERT_NE(stats_of(proxy().backend_stats(), "b2"), nullptr);
    for (int i = 0; i < 4; ++i) fetch(port(), "/");
    EXPECT_EQ(backend(1).stats().requests, 2u);

    const auto file = saved();
    ASSERT_EQ(file["groups"][0]["backends"].size(), 2u);
    EXPECT_EQ(file["groups"][0]["backends"][1]["id"], "b2");
    EXPECT_EQ(file["groups"][0]["backends"][1]["port"], p2);
    EXPECT_EQ(file["groups"][0]["backends"][1]["drain"], "keep");
    // The file watcher sees the save but recognizes the active config's content.
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(proxy().stats().reloads_accepted, 1u);
    EXPECT_EQ(proxy().stats().reloads_rejected, 0u);
    bool logged = false;
    for (const auto& e : proxy().recent_events()) logged = logged || e.type == "admin_edit";
    EXPECT_TRUE(logged);
}

TEST_F(AdminTest, SavedFileKeepsItsFieldOrder) {
    const auto p1 = start_backend({}, "b1");
    start_from_file({p1});
    const std::string before = read_text(path_);
    ASSERT_TRUE(proxy().admin_update_backend(backend_edit("b1", p1, 4)).ok);
    const std::string after = read_text(path_);
    // Same top-level keys in the same order.
    std::vector<std::string> keys_before;
    std::vector<std::string> keys_after;
    const auto doc_before = nlohmann::ordered_json::parse(before);  // items() refers into the document
    const auto doc_after = nlohmann::ordered_json::parse(after);
    for (const auto& [k, v] : doc_before.items()) keys_before.push_back(k);
    for (const auto& [k, v] : doc_after.items()) keys_after.push_back(k);
    EXPECT_EQ(keys_after, keys_before);
    EXPECT_EQ(saved()["groups"][0]["backends"][0]["weight"], 4);
    EXPECT_EQ(stats_of(proxy().backend_stats(), "b1")->weight, 4u);
}

// Every admin action goes through the validation path: an invalid edit changes nothing,
// neither the running proxy nor the file, and the error names the field.
TEST_F(AdminTest, InvalidEditIsRejectedAndChangesNothing) {
    const auto p1 = start_backend({}, "b1");
    start_from_file({p1});
    const std::string before = read_text(path_);

    auto r = proxy().admin_add_backend(backend_edit("b1", 9999));  // duplicate id
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("/groups/0/backends/1/id: duplicate backend id"), std::string::npos) << r.error;
    r = proxy().admin_add_backend(backend_edit("b 2", 9999));  // bad id
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("/groups/0/backends/1/id"), std::string::npos) << r.error;
    r = proxy().admin_add_backend(backend_edit("b2", 0));  // bad port
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("/groups/0/backends/1/port: must be between 1 and 65535"), std::string::npos) << r.error;
    auto in_nowhere = backend_edit("b3", 9999);
    in_nowhere.group = "nope";
    r = proxy().admin_add_backend(in_nowhere);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "no group \"nope\"");
    r = proxy().admin_remove_backend("ghost");
    EXPECT_EQ(r.error, "no backend \"ghost\"");
    r = proxy().admin_remove_backend("b1");  // the group would have no backends left
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("must contain at least one backend"), std::string::npos) << r.error;

    EXPECT_EQ(read_text(path_), before);
    EXPECT_EQ(proxy().backend_stats().size(), 1u);
    EXPECT_EQ(fetch(port(), "/").status, 200);
}

// Plan IV.17 done (engine half): removing a backend from the console under load drops nothing.
TEST_F(AdminTest, RemovingABackendUnderLoadDropsNothing) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_from_file({p1, p2});
    std::atomic<bool> stop{false};
    std::atomic<int> ok{0};
    std::atomic<int> failed{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t) {
        clients.emplace_back([&] {
            TestClient c(port());
            while (!stop) {
                ClientResponse r;
                if (c.send(get_request("/"))) r = c.read_response();
                (r.status == 200 && r.end == ClientResponse::End::Complete ? ok : failed)++;
            }
        });
    }
    ASSERT_TRUE(eventually([&] { return backend(1).stats().requests > 100; }));
    const auto r = proxy().admin_remove_backend("b2");
    ASSERT_TRUE(r.ok) << r.error;
    std::this_thread::sleep_for(50ms);
    const auto b2_after = backend(1).stats().requests;
    const int ok_then = ok;
    ASSERT_TRUE(eventually([&] { return ok > ok_then + 200; }));
    stop = true;
    for (auto& t : clients) t.join();
    EXPECT_EQ(failed.load(), 0);
    EXPECT_EQ(backend(1).stats().requests, b2_after);
    EXPECT_EQ(saved()["groups"][0]["backends"].size(), 1u);
}

// A drain started from the console is saved ("start"), so a restart keeps it; returning the
// backend saves "keep".
TEST_F(AdminTest, DrainAndReturnToServiceAreSaved) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_from_file({p1, p2});
    ASSERT_TRUE(proxy().admin_drain_backend("b2").ok);
    EXPECT_EQ(saved()["groups"][0]["backends"][1]["drain"], "start");
    ASSERT_TRUE(eventually([&] { return stats_of(proxy().backend_stats(), "b2")->state == BackendState::Drained; }));
    for (int i = 0; i < 4; ++i) EXPECT_EQ(fetch(port(), "/").header("x-backend-id"), "b1");

    const auto r = proxy().admin_undrain_backend("b2");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(saved()["groups"][0]["backends"][1]["drain"], "keep");
    EXPECT_EQ(stats_of(proxy().backend_stats(), "b2")->state, BackendState::Healthy);
    int to_b2 = 0;
    for (int i = 0; i < 4; ++i) to_b2 += fetch(port(), "/").header("x-backend-id") == "b2" ? 1 : 0;
    EXPECT_EQ(to_b2, 2);
}

TEST_F(AdminTest, RoutingRulesEditedFromTheConsole) {
    const auto web = start_backend({}, "web-1");
    const auto api = start_backend({}, "api-1");
    start_from_file({web}, [&](nlohmann::json& j) {
        auto group = j["groups"][0];
        group["name"] = "api";
        group["backends"] = nlohmann::json::array(
            {{{"id", "api-1"}, {"address", "127.0.0.1"}, {"port", api}, {"weight", 1}, {"drain", "keep"}}});
        j["groups"].push_back(group);
    });
    lb::RoutingConfig routing;
    routing.default_group = "web";
    lb::RouteRule rule;
    rule.id = "api";
    rule.type = lb::RouteRule::Type::PathPrefix;
    rule.value = "/api";
    rule.group = "api";
    routing.rules.push_back(rule);
    lb::RouteRule beta;
    beta.id = "beta";
    beta.type = lb::RouteRule::Type::Cookie;
    beta.field = "beta";
    beta.group = "api";  // value empty: presence
    routing.rules.push_back(beta);
    ASSERT_TRUE(proxy().admin_set_routing(routing).ok);
    EXPECT_EQ(fetch(port(), "/api/x").header("x-backend-id"), "api-1");
    EXPECT_EQ(fetch(port(), "/x", "Cookie: beta=1\r\n").header("x-backend-id"), "api-1");
    EXPECT_EQ(fetch(port(), "/x").header("x-backend-id"), "web-1");
    const auto file = saved();
    ASSERT_EQ(file["routing"]["rules"].size(), 2u);
    EXPECT_TRUE(file["routing"]["rules"][0]["field"].is_null());
    EXPECT_EQ(file["routing"]["rules"][0]["value"], "/api");
    EXPECT_TRUE(file["routing"]["rules"][1]["value"].is_null());

    rule.group = "missing";
    routing.rules = {rule};
    const auto r = proxy().admin_set_routing(routing);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("/routing/rules/0/group: unknown group \"missing\""), std::string::npos) << r.error;
    EXPECT_EQ(fetch(port(), "/api/x").header("x-backend-id"), "api-1");  // still the previous rules
}

// The console never overwrites changes it has not seen: if the file on disk is not the
// active config (edited and rejected, or not yet applied), admin edits refuse.
TEST_F(AdminTest, RefusesToOverwriteAFileThatIsNotTheActiveConfig) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_from_file({p1});
    std::ofstream(path_, std::ios::binary) << "{ \"half\": ";  // an editor's unfinished save
    ASSERT_TRUE(eventually([&] { return proxy().stats().reloads_rejected == 1; }));
    const auto r = proxy().admin_add_backend(backend_edit("b2", p2));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("is not the active config"), std::string::npos) << r.error;
    EXPECT_EQ(read_text(path_), "{ \"half\": ");
}

TEST_F(ProxyTest, AdminEditsNeedARegisteredConfigFile) {
    start_proxy({start_backend()});
    const auto r = engine().admin_remove_backend("b1");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "admin edits are saved to the config file, and none is registered");
}

// The dashboard snapshot carries the active config for the admin dialogs.
TEST_F(AdminTest, SnapshotCarriesTheActiveConfigForTheConsole) {
    struct Sink final : lb::SnapshotSink {
        void on_snapshot(std::unique_ptr<lb::DashboardSnapshot> s) noexcept override {
            std::lock_guard lock(m);
            last = std::move(s);
        }
        std::mutex m;
        std::unique_ptr<lb::DashboardSnapshot> last;
    } sink;
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto j = lbtest::make_proxy_config_json({p1}, [](nlohmann::json& c) { c["dashboard"]["publish_interval_ms"] = 50; });
    std::ofstream(path_, std::ios::binary) << j.dump(2);
    engine_ = std::make_unique<lb::Engine>(lb::load_config_file(path_).snapshot);
    engine_->set_snapshot_sink(&sink);
    std::string error;
    ASSERT_TRUE(engine_->start(&error)) << error;
    ASSERT_TRUE(engine_->watch_config_file(path_, &error)) << error;  // watch_file is false: registered only
    ASSERT_TRUE(proxy().admin_add_backend(backend_edit("b2", p2)).ok);
    ASSERT_TRUE(eventually([&] {
        std::lock_guard lock(sink.m);
        return sink.last && sink.last->admin_available && sink.last->config &&
               sink.last->config->groups[0].backends.size() == 2;
    }));
}
