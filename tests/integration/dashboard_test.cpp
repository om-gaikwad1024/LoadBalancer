#include <gtest/gtest.h>

#include <windows.h>
#include <commctrl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "resource.h"  // src/app: the dashboard's control ids
#include "test_http_client.h"
#include "test_process.h"

using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
namespace fs = std::filesystem;

namespace {

class CollectingSink final : public lb::SnapshotSink {
public:
    void on_snapshot(std::unique_ptr<lb::DashboardSnapshot> s) noexcept override {
        std::lock_guard lock(mutex_);
        threads_.insert(::GetCurrentThreadId());
        snapshots_.push_back(std::move(s));
    }

    std::size_t count() {
        std::lock_guard lock(mutex_);
        return snapshots_.size();
    }

    template <typename Fn>
    void with(Fn&& fn) {
        std::lock_guard lock(mutex_);
        fn(snapshots_, threads_);
    }

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<lb::DashboardSnapshot>> snapshots_;
    std::set<DWORD> threads_;
};

void fast_dashboard(nlohmann::json& j) {
    j["dashboard"]["publish_interval_ms"] = 100;
    j["groups"][0]["health"]["interval_ms"] = 100;
    j["groups"][0]["health"]["timeout_ms"] = 100;
}

// ---- Reading another process's dashboard through Win32 -------------------------------

struct FindData {
    DWORD pid;
    HWND found;
};

HWND find_dialog(DWORD pid) {
    FindData data{pid, nullptr};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* d = reinterpret_cast<FindData*>(param);
            DWORD owner = 0;
            ::GetWindowThreadProcessId(hwnd, &owner);
            wchar_t cls[64] = {};
            ::GetClassNameW(hwnd, cls, 64);
            if (owner == d->pid && std::wstring(cls) == L"#32770" && ::GetDlgItem(hwnd, IDC_BACKENDS) != nullptr) {
                d->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));
    return data.found;
}

// WM_GETTEXT and LVM_GETITEMCOUNT are marshalled across processes by the system.
std::wstring text_of(HWND dialog, int id) {
    wchar_t buf[4096] = {};
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(::GetDlgItem(dialog, id), WM_GETTEXT, 4096, reinterpret_cast<LPARAM>(buf), SMTO_ABORTIFHUNG,
                          2000, &result);
    return buf;
}

int rows_of(HWND dialog, int id) {
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(::GetDlgItem(dialog, id), LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 2000, &result);
    return static_cast<int>(result);
}

bool contains(const std::wstring& text, const std::wstring& part) { return text.find(part) != std::wstring::npos; }

}  // namespace

// Plan IV.17 / V: the engine publishes copied snapshots on its own thread; the UI only
// receives them.
TEST_F(ProxyTest, EnginePublishesCopiedSnapshotsOnItsOwnThread) {
    const auto port = start_backend();
    CollectingSink sink;
    lb::Engine engine(lbtest::make_proxy_config({port}, fast_dashboard));
    engine.set_snapshot_sink(&sink);
    std::string error;
    ASSERT_TRUE(engine.start(&error)) << error;
    const std::uint16_t listen_port = engine.listen_port();
    TestClient c(listen_port);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    ASSERT_TRUE(eventually([&] { return sink.count() >= 6; }));
    engine.stop();
    const std::size_t after_stop = sink.count();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(sink.count(), after_stop);  // nothing is published once shutdown begins

    sink.with([&](auto& snapshots, auto& threads) {
        EXPECT_EQ(threads.size(), 1u);
        EXPECT_EQ(threads.count(::GetCurrentThreadId()), 0u);  // never the caller's thread
        std::uint64_t last_event = 0;
        int engine_started = 0;
        for (std::size_t i = 0; i < snapshots.size(); ++i) {
            EXPECT_EQ(snapshots[i]->sequence, i + 1);
            for (const auto& e : snapshots[i]->new_events) {
                EXPECT_GT(e.sequence, last_event);  // each event is delivered once, in order
                last_event = e.sequence;
                engine_started += e.type == "engine_started" ? 1 : 0;
            }
        }
        EXPECT_EQ(engine_started, 1);
        const auto& last = *snapshots.back();
        EXPECT_EQ(last.listen_port, listen_port);
        ASSERT_EQ(last.backends.size(), 1u);
        EXPECT_EQ(last.backends[0].requests, 10u);
        EXPECT_EQ(last.metrics.system.total_since_start.count, 10u);
        EXPECT_GT(last.uptime_seconds, 0.0);
    });
}

// Plan IV.17 Done (phase 1 part): the operator watches health, percentiles and the event
// log update live. Drives the real LoadBalancer.exe and reads its controls.
TEST_F(ProxyTest, DashboardShowsHealthPercentilesAndEventsLive) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const fs::path config = fs::temp_directory_path() / "lb-dashboard-test.json";
    std::ofstream(config) << lbtest::make_proxy_config_json({p1, p2}, fast_dashboard).dump(2);

    lbtest::ChildProcess app;
    ASSERT_TRUE(app.start(LB_APP_EXE, L"--config \"" + config.wstring() + L"\" --minimized"));
    HWND dlg = nullptr;
    ASSERT_TRUE(eventually([&] { return (dlg = find_dialog(app.pid())) != nullptr; }, std::chrono::seconds(15)));

    // Status line, backend list.
    ASSERT_TRUE(eventually([&] { return contains(text_of(dlg, IDC_STATUS), L"Listening on 127.0.0.1:"); }));
    ASSERT_TRUE(eventually([&] { return rows_of(dlg, IDC_BACKENDS) == 2; }));
    ASSERT_TRUE(eventually([&] { return contains(text_of(dlg, IDC_STATUS), L"2 healthy, 0 unhealthy"); }));
    const std::wstring status = text_of(dlg, IDC_STATUS);
    const auto colon = status.find(L"127.0.0.1:") + 10;
    const auto proxy_port = static_cast<std::uint16_t>(std::stoi(status.substr(colon)));

    // Percentiles appear once traffic flows.
    EXPECT_TRUE(contains(text_of(dlg, IDC_P99_LABEL), L"–"));
    TestClient c(proxy_port);
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(c.send(get_request("/dash")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    EXPECT_TRUE(eventually([&] { return contains(text_of(dlg, IDC_P99_LABEL), L" ms"); }));
    EXPECT_TRUE(eventually([&] { return contains(text_of(dlg, IDC_MAX_LABEL), L" ms"); }));
    EXPECT_TRUE(eventually([&] { return contains(text_of(dlg, IDC_LATENCY_DETAIL), L"30 requests"); }));

    // Health and the event list follow a backend kill.
    ASSERT_TRUE(eventually([&] { return rows_of(dlg, IDC_EVENTS) >= 1; }));  // engine_started
    const int events_before = rows_of(dlg, IDC_EVENTS);
    backend(0).stop();
    EXPECT_TRUE(eventually([&] { return contains(text_of(dlg, IDC_STATUS), L"1 healthy, 1 unhealthy"); }));
    EXPECT_TRUE(eventually([&] { return rows_of(dlg, IDC_EVENTS) > events_before; }));
    std::wprintf(L"[ dashboard ] %ls\n[ dashboard ] %ls | %ls\n", text_of(dlg, IDC_STATUS).c_str(),
                 text_of(dlg, IDC_P99_LABEL).c_str(), text_of(dlg, IDC_MAX_LABEL).c_str());

    // Closing the window stops the proxy cleanly.
    ::PostMessageW(dlg, WM_CLOSE, 0, 0);
    DWORD exit_code = 1;
    ASSERT_TRUE(app.wait_for_exit(std::chrono::seconds(15), &exit_code));
    EXPECT_EQ(exit_code, 0u);
    TestClient after(proxy_port);
    EXPECT_FALSE(after.connected());
    fs::remove(config);
}
