// Step 2.1 (plan IV.14): hot reload of the running engine. Validate everything, then swap;
// in-flight requests keep their snapshot, new requests see the complete new one; a broken
// or restart-only config changes nothing. The file watcher debounces multi-write saves
// and skips content equal to the active config.

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

using Members = std::vector<std::pair<std::string, std::uint16_t>>;  // backend id, port

// The test config with an explicit backend list (ids are kept across reloads).
nlohmann::json config_json(const Members& members, const std::function<void(nlohmann::json&)>& tweak = {}) {
    auto j = lbtest::make_proxy_config_json({});
    auto backends = nlohmann::json::array();
    for (const auto& [id, port] : members) {
        backends.push_back({{"id", id}, {"address", "127.0.0.1"}, {"port", port}, {"weight", 1}, {"drain", "keep"}});
    }
    j["groups"][0]["backends"] = backends;
    if (tweak) tweak(j);
    return j;
}

std::shared_ptr<const lb::ConfigSnapshot> config(const Members& members,
                                                 const std::function<void(nlohmann::json&)>& tweak = {}) {
    auto loaded = lb::parse_config(config_json(members, tweak).dump());
    for (const auto& e : loaded.errors) ADD_FAILURE() << "test config rejected: " << lb::to_string(e);
    return loaded.snapshot;
}

const lb::BackendStats* find_stats(const std::vector<lb::BackendStats>& all, const std::string& id) {
    for (const auto& b : all) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

std::size_t count_events(lb::Engine& engine, const std::string& type) {
    std::size_t n = 0;
    for (const auto& e : engine.recent_events()) n += e.type == type ? 1 : 0;
    return n;
}

// Keep-alive clients sending requests back to back until stop(). Anything but a complete
// 200 counts as a failure: an error status, a reset, a close or a timeout.
class Load {
public:
    Load(std::uint16_t port, int clients) {
        for (int t = 0; t < clients; ++t) {
            threads_.emplace_back([this, port] {
                auto c = std::make_unique<TestClient>(port);
                while (!stop_.load()) {
                    lbtest::ClientResponse r;
                    if (c->connected() && c->send(get_request("/load"))) r = c->read_response();
                    if (r.status == 200 && r.end == lbtest::ClientResponse::End::Complete) {
                        ++ok_;
                        continue;
                    }
                    ++failed_;
                    {
                        std::lock_guard lock(mutex_);
                        if (first_failure_.empty()) {
                            first_failure_ = "status " + std::to_string(r.status) + ", end " +
                                             std::to_string(static_cast<int>(r.end));
                        }
                    }
                    c = std::make_unique<TestClient>(port);
                }
            });
        }
    }
    ~Load() { stop(); }

    void stop() {
        stop_ = true;
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }
    int ok() const { return ok_.load(); }
    int failed() const { return failed_.load(); }
    std::string first_failure() {
        std::lock_guard lock(mutex_);
        return first_failure_;
    }

private:
    std::atomic<bool> stop_{false};
    std::atomic<int> ok_{0};
    std::atomic<int> failed_{0};
    std::mutex mutex_;
    std::string first_failure_;
    std::vector<std::thread> threads_;
};

class ReloadTest : public ProxyTest {
protected:
    void start(const Members& members, const std::function<void(nlohmann::json&)>& tweak = {}) {
        engine_ = std::make_unique<lb::Engine>(config(members, tweak));
        std::string error;
        ASSERT_TRUE(engine_->start(&error)) << error;
    }
    lb::Engine& proxy() { return *engine_; }
    std::uint16_t port() const { return engine_->listen_port(); }

    void TearDown() override {
        if (engine_) engine_->stop();
        ProxyTest::TearDown();
    }

    std::unique_ptr<lb::Engine> engine_;
};

}  // namespace

// Plan IV.14 done: adding or removing a backend (and changing the strategy) under load
// takes effect for new requests with zero dropped connections.
TEST_F(ReloadTest, AddAndRemoveBackendsUnderLoadWithZeroErrors) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start({{"b1", p1}});
    Load load(port(), 4);
    ASSERT_TRUE(eventually([&] { return load.ok() > 200; }));

    // Add b2: new requests are spread over both.
    auto r = proxy().reload(config({{"b1", p1}, {"b2", p2}}));
    ASSERT_TRUE(r.accepted) << (r.errors.empty() ? "" : r.errors.front());
    EXPECT_EQ(r.summary, "added b2");
    ASSERT_TRUE(eventually([&] { return backend(1).stats().requests > 200; }));

    // Change the strategy under load.
    r = proxy().reload(config({{"b1", p1}, {"b2", p2}}, [](auto& j) { j["groups"][0]["strategy"] = "least_connections"; }));
    ASSERT_TRUE(r.accepted);
    const int before_removal = load.ok();
    ASSERT_TRUE(eventually([&] { return load.ok() > before_removal + 200; }));

    // Remove b1: once in-flight requests finish, b1 gets nothing more.
    r = proxy().reload(config({{"b2", p2}}));
    ASSERT_TRUE(r.accepted);
    EXPECT_EQ(r.summary, "removed b1");
    std::this_thread::sleep_for(100ms);  // requests that started before the swap finish
    const auto b1_after = backend(0).stats().requests;
    const auto b2_after = backend(1).stats().requests;
    ASSERT_TRUE(eventually([&] { return backend(1).stats().requests > b2_after + 200; }));
    EXPECT_EQ(backend(0).stats().requests, b1_after);
    load.stop();

    EXPECT_EQ(load.failed(), 0) << load.first_failure();
    EXPECT_GT(load.ok(), 600);
    const auto stats = proxy().backend_stats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats[0].id, "b2");
    EXPECT_EQ(proxy().stats().reloads_accepted, 3u);
    EXPECT_EQ(proxy().stats().reloads_rejected, 0u);
    EXPECT_EQ(proxy().stats().error_responses, 0u);
    EXPECT_EQ(count_events(proxy(), "config_reload_accepted"), 3u);
}

// Plan IV.14 done: a deliberately broken config is rejected, logged, and has no effect.
TEST_F(ReloadTest, BrokenConfigIsRejectedLoggedAndHasNoEffect) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start({{"b1", p1}});
    Load load(port(), 2);

    // Not JSON at all, then valid JSON with an invalid value, then a missing field.
    auto r = proxy().reload_from_text("{ \"listen\": ", "api");
    EXPECT_FALSE(r.accepted);
    ASSERT_FALSE(r.errors.empty());
    auto bad = config_json({{"b1", p1}, {"b2", p2}}, [](auto& j) { j["groups"][0]["strategy"] = "fastest"; });
    r = proxy().reload_from_text(bad.dump(), "api");
    EXPECT_FALSE(r.accepted);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors.front().find("/groups/0/strategy"), std::string::npos) << r.errors.front();
    bad = config_json({{"b1", p1}, {"b2", p2}}, [](auto& j) { j.erase("pool"); });
    r = proxy().reload_from_text(bad.dump(), "api");
    EXPECT_FALSE(r.accepted);

    // Still the old config: b2 was never added, and traffic is unaffected.
    std::this_thread::sleep_for(200ms);
    load.stop();
    EXPECT_EQ(load.failed(), 0) << load.first_failure();
    EXPECT_GT(load.ok(), 0);
    EXPECT_EQ(backend(1).stats().requests, 0u);
    EXPECT_EQ(proxy().backend_stats().size(), 1u);
    EXPECT_EQ(proxy().stats().reloads_rejected, 3u);
    EXPECT_EQ(proxy().stats().reloads_accepted, 0u);
    EXPECT_EQ(count_events(proxy(), "config_reload_rejected"), 3u);
    for (const auto& e : proxy().recent_events()) {
        if (e.type == "config_reload_rejected") {
            EXPECT_NE(e.message.find("still running the previous config"), std::string::npos) << e.message;
        }
    }
}

TEST_F(ReloadTest, RestartOnlyChangeRejectsTheWholeReload) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start({{"b1", p1}});
    // A valid config that adds b2 but also changes the worker count and the metrics window.
    const auto r = proxy().reload(config({{"b1", p1}, {"b2", p2}}, [](auto& j) {
        j["workers"]["threads"] = 3;
        j["metrics"]["slice_ms"] = 500;
    }));
    EXPECT_FALSE(r.accepted);
    ASSERT_EQ(r.errors.size(), 2u);
    EXPECT_NE(r.errors[0].find("workers.threads"), std::string::npos) << r.errors[0];
    EXPECT_NE(r.errors[1].find("metrics.slice_ms"), std::string::npos) << r.errors[1];
    EXPECT_EQ(proxy().backend_stats().size(), 1u);  // not half-applied: b2 is not there
    for (int i = 0; i < 4; ++i) EXPECT_EQ(fetch(port(), "/").header("x-backend-id"), "b1");
}

// Plan IV.12: a reload never silently un-drains.
TEST_F(ReloadTest, DrainedBackendStaysDrainedAcrossReloads) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start({{"b1", p1}, {"b2", p2}});
    ASSERT_TRUE(proxy().set_backend_state("b1", BackendState::Draining));
    const auto r = proxy().reload(config({{"b1", p1}, {"b2", p2}}, [](auto& j) {
        j["groups"][0]["backends"][0]["weight"] = 5;
        j["pool"]["max_idle_per_backend"] = 4;
    }));
    ASSERT_TRUE(r.accepted);
    EXPECT_EQ(r.summary, "reweighted b1");
    const auto stats = proxy().backend_stats();
    ASSERT_NE(find_stats(stats, "b1"), nullptr);
    EXPECT_EQ(find_stats(stats, "b1")->state, BackendState::Draining);
    EXPECT_EQ(find_stats(stats, "b1")->weight, 5u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(fetch(port(), "/").header("x-backend-id"), "b2");
    EXPECT_EQ(backend(0).stats().requests, 0u);
}

TEST_F(ReloadTest, TimeoutChangeAppliesToNewRequests) {
    mock::MockFaults slow;
    slow.latency_ms = 300;
    const auto p1 = start_backend(slow, "b1");
    start({{"b1", p1}});
    EXPECT_EQ(fetch(port(), "/").status, 200);
    ASSERT_TRUE(proxy().reload(config({{"b1", p1}}, [](auto& j) { j["timeouts"]["backend_response_ms"] = 100; }))
                    .accepted);
    EXPECT_EQ(fetch(port(), "/").status, 504);
    ASSERT_TRUE(proxy().reload(config({{"b1", p1}})).accepted);
    EXPECT_EQ(fetch(port(), "/").status, 200);
}

// Plan IV.14: in-flight requests keep their old snapshot.
TEST_F(ReloadTest, InFlightRequestFinishesOnTheBackendItStartedWith) {
    mock::MockFaults slow;
    slow.latency_ms = 600;
    const auto p1 = start_backend(slow, "b1");
    const auto p2 = start_backend({}, "b2");
    start({{"b1", p1}});

    lbtest::ClientResponse in_flight;
    std::thread t([&] { in_flight = fetch(port(), "/slow"); });
    ASSERT_TRUE(eventually([&] {
        const auto s = proxy().backend_stats();
        return !s.empty() && s[0].in_flight == 1;
    }));
    ASSERT_TRUE(proxy().reload(config({{"b2", p2}})).accepted);  // b1 removed while busy
    const auto fresh = fetch(port(), "/");
    EXPECT_EQ(fresh.status, 200);
    EXPECT_EQ(fresh.header("x-backend-id"), "b2");
    t.join();
    EXPECT_EQ(in_flight.status, 200);
    EXPECT_EQ(in_flight.end, lbtest::ClientResponse::End::Complete);
    EXPECT_EQ(in_flight.header("x-backend-id"), "b1");
}

TEST_F(ReloadTest, NewBackendIdsBeyondTheMetricsCapacityAreRejected) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    const auto set_cap = [](auto& j) { j["metrics"]["max_backend_series"] = 3; };
    start({{"b1", p1}, {"b2", p2}}, set_cap);
    ASSERT_TRUE(proxy().reload(config({{"b1", p1}, {"b2", p2}, {"b3", p3}}, set_cap)).accepted);
    ASSERT_TRUE(proxy().reload(config({{"b1", p1}, {"b2", p2}}, set_cap)).accepted);
    ASSERT_TRUE(proxy().reload(config({{"b1", p1}, {"b2", p2}, {"b3", p3}}, set_cap)).accepted);  // b3 again: its series
    const auto r = proxy().reload(config({{"b1", p1}, {"b2", p2}, {"b4", p3}}, set_cap));  // a 4th id
    EXPECT_FALSE(r.accepted);
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_NE(r.errors[0].find("metrics.max_backend_series"), std::string::npos) << r.errors[0];

    // Latency for a backend added by reload lands in its own series.
    // (A request is recorded when its last write completes, which can be just after the
    // client has read the response, so poll.)
    for (int i = 0; i < 6; ++i) EXPECT_EQ(fetch(port(), "/").status, 200);
    EXPECT_TRUE(eventually([&] { return proxy().metrics().system.total_since_start.count == 6; }));
    const auto m = proxy().metrics();
    ASSERT_EQ(m.backends.size(), 3u);
    EXPECT_EQ(m.backends[2].id, "b3");
    EXPECT_EQ(m.backends[2].total_since_start.count, 2u);
}

// The file watcher (plan IV.14): debounced, content-hash checked, survives garbage, and
// sees saves that replace the file.
class ConfigFileTest : public ReloadTest {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("lb-reload-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount64()));
        std::filesystem::create_directories(dir_);
        path_ = dir_ / "lb.json";
    }
    void TearDown() override {
        ReloadTest::TearDown();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    static void watch(nlohmann::json& j) {
        j["config_reload"]["watch_file"] = true;
        j["config_reload"]["debounce_ms"] = 150;
    }

    void write(const std::string& text) const { std::ofstream(path_, std::ios::binary | std::ios::trunc) << text; }

    std::filesystem::path dir_;
    std::filesystem::path path_;
};

TEST_F(ConfigFileTest, WatcherReloadsOnceAfterAMultiWriteSaveAndSkipsUnchangedContent) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const std::string first = config_json({{"b1", p1}}, watch).dump(2);
    write(first);
    start({{"b1", p1}}, watch);
    std::string error;
    ASSERT_TRUE(proxy().watch_config_file(path_, &error)) << error;
    EXPECT_EQ(count_events(proxy(), "config_watch_started"), 1u);

    // Saved again with the same content (what a GUI save looks like to the watcher).
    write(first);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(proxy().stats().reloads_accepted, 0u);
    EXPECT_EQ(proxy().stats().reloads_rejected, 0u);

    // An editor saving in several writes: the half-written file is never parsed.
    const std::string second = config_json({{"b1", p1}, {"b2", p2}}, watch).dump(2);
    {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << second.substr(0, second.size() / 2) << std::flush;
        std::this_thread::sleep_for(40ms);
        out << second.substr(second.size() / 2) << std::flush;
    }
    ASSERT_TRUE(eventually([&] { return proxy().stats().reloads_accepted == 1; }));
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(proxy().stats().reloads_accepted, 1u);
    EXPECT_EQ(proxy().stats().reloads_rejected, 0u);
    EXPECT_EQ(proxy().backend_stats().size(), 2u);
}

TEST_F(ConfigFileTest, WatcherRejectsGarbageKeepsServingAndSeesReplacedFiles) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    write(config_json({{"b1", p1}}, watch).dump(2));
    start({{"b1", p1}}, watch);
    std::string error;
    ASSERT_TRUE(proxy().watch_config_file(path_, &error)) << error;
    EXPECT_FALSE(proxy().watch_config_file(path_, &error));  // one watcher per engine
    EXPECT_EQ(error, "already watching a config file");

    write("this is not a config");
    ASSERT_TRUE(eventually([&] { return proxy().stats().reloads_rejected == 1; }));
    EXPECT_EQ(fetch(port(), "/").status, 200);
    EXPECT_EQ(proxy().backend_stats().size(), 1u);

    // Save by replacing: write a temp file, then rename it over the config.
    const auto temp = dir_ / "lb.json.tmp";
    std::ofstream(temp, std::ios::binary) << config_json({{"b1", p1}, {"b2", p2}}, watch).dump(2);
    ASSERT_TRUE(::MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING));
    ASSERT_TRUE(eventually([&] { return proxy().stats().reloads_accepted == 1; }));
    EXPECT_EQ(proxy().backend_stats().size(), 2u);
    EXPECT_EQ(count_events(proxy(), "config_reload_rejected"), 1u);
    EXPECT_EQ(count_events(proxy(), "config_reload_accepted"), 1u);
}

TEST_F(ConfigFileTest, WatchingIsOffWhenTheConfigSaysSo) {
    const auto p1 = start_backend({}, "b1");
    write(config_json({{"b1", p1}}).dump(2));  // watch_file false
    start({{"b1", p1}});
    std::string error;
    ASSERT_TRUE(proxy().watch_config_file(path_, &error)) << error;
    EXPECT_EQ(count_events(proxy(), "config_watch_started"), 0u);
    write("garbage");
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(proxy().stats().reloads_rejected, 0u);
    // Still reloadable on request.
    const auto r = proxy().reload_from_file(path_);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(proxy().stats().reloads_rejected, 1u);
}
