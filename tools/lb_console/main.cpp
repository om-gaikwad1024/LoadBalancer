// Headless host for the proxy engine: same engine as the MFC app, without a GUI.
// Used for load tests, soak runs and scripted scenarios.
//   lb_console --config config\lb.example.json
// Commands on stdin (one per line), for scripts:
//   metrics <file>   write metrics, engine stats and backend states as JSON to <file>
//   reload           reload the config file now (the watcher does this on change)
//   quit             stop gracefully (same as Ctrl+C)

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/config_loader.h"
#include "engine.h"

namespace {

HANDLE g_stop_event = nullptr;

BOOL WINAPI on_console_ctrl(DWORD) {
    ::SetEvent(g_stop_event);
    return TRUE;
}

nlohmann::json latency_json(const lb::LatencyStats& s) {
    return {{"count", s.count}, {"mean_ms", s.mean_ms}, {"p50_ms", s.p50_ms},
            {"p95_ms", s.p95_ms}, {"p99_ms", s.p99_ms}, {"max_ms", s.max_ms}};
}

nlohmann::json series_json(const lb::SeriesMetrics& m) {
    return {{"id", m.id},
            {"total_window", latency_json(m.total_window)},
            {"total_since_start", latency_json(m.total_since_start)},
            {"backend_window", latency_json(m.backend_window)},
            {"backend_since_start", latency_json(m.backend_since_start)},
            {"status_window", m.status_window},
            {"status_since_start", m.status_since_start},
            {"requests_per_second", m.requests_per_second},
            {"error_rate", m.error_rate}};
}

void write_metrics(const lb::Engine& engine, const std::string& path) {
    const auto m = engine.metrics();
    const auto s = engine.stats();
    nlohmann::json j;
    j["window_seconds"] = m.window_seconds;
    j["system"] = series_json(m.system);
    j["backends"] = nlohmann::json::array();
    for (const auto& b : m.backends) j["backends"].push_back(series_json(b));
    j["stats"] = {{"connections_accepted", s.connections_accepted},
                  {"connections_active", s.connections_active},
                  {"connections_rejected", s.connections_rejected},
                  {"requests_completed", s.requests_completed},
                  {"error_responses", s.error_responses},
                  {"backend_connections_opened", s.backend_connections_opened},
                  {"backend_connections_reused", s.backend_connections_reused},
                  {"stale_retries", s.stale_retries},
                  {"pool_rejections", s.pool_rejections},
                  {"no_backend_available", s.no_backend_available},
                  {"backends_marked_down", s.backends_marked_down},
                  {"backends_marked_up", s.backends_marked_up},
                  {"client_timeouts", s.client_timeouts},
                  {"backend_timeouts", s.backend_timeouts},
                  {"events_dropped", s.events_dropped},
                  {"reloads_accepted", s.reloads_accepted},
                  {"reloads_rejected", s.reloads_rejected},
                  {"sticky_entries", s.sticky_entries},
                  {"sticky_hits", s.sticky_hits},
                  {"sticky_assignments", s.sticky_assignments},
                  {"sticky_reassignments", s.sticky_reassignments},
                  {"sticky_not_stored", s.sticky_not_stored},
                  {"drains_started", s.drains_started},
                  {"drains_completed", s.drains_completed},
                  {"drains_timed_out", s.drains_timed_out},
                  {"drain_aborted_requests", s.drain_aborted_requests}};
    j["backend_states"] = nlohmann::json::array();
    for (const auto& b : engine.backend_stats()) {
        j["backend_states"].push_back({{"id", b.id},
                                       {"state", std::string(lb::to_string(b.state))},
                                       {"requests", b.requests},
                                       {"failures", b.failures},
                                       {"response_time_ms", b.response_time_ms},
                                       {"connections_opened", b.connections_opened},
                                       {"connections_reused", b.connections_reused}});
    }
    std::ofstream(path) << j.dump(2);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string_view(argv[i]) == "--config") config_path = argv[i + 1];
    }
    if (config_path.empty()) {
        std::fprintf(stderr, "usage: lb_console --config <file.json>\n");
        return 2;
    }

    const auto loaded = lb::load_config_file(config_path);
    if (!loaded.ok()) {
        std::fprintf(stderr, "config rejected (%zu error%s):\n", loaded.errors.size(), loaded.errors.size() == 1 ? "" : "s");
        for (const auto& e : loaded.errors) std::fprintf(stderr, "  %s\n", lb::to_string(e).c_str());
        return 1;
    }

    lb::Engine engine(loaded.snapshot);
    std::string error;
    if (!engine.start(&error)) {
        std::fprintf(stderr, "engine failed to start: %s\n", error.c_str());
        return 1;
    }
    std::printf("lb_console %s listening on %s:%u with %u worker threads (Ctrl+C to stop)\n",
                std::string(lb::engine_version()).c_str(), loaded.snapshot->listen.address.c_str(), engine.listen_port(),
                engine.worker_threads());
    if (!engine.watch_config_file(config_path, &error)) {
        std::fprintf(stderr, "config changes will not be picked up: %s\n", error.c_str());
    }
    std::fflush(stdout);

    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    // Script commands. A closed stdin simply ends this thread; the proxy keeps running.
    // Raw ReadFile, not std::cin: a thread blocked inside the CRT would hold its stdin
    // lock while the process exits.
    std::thread commands([&engine, &config_path] {
        const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
        std::string buffer;
        char chunk[512];
        DWORD n = 0;
        while (in != nullptr && in != INVALID_HANDLE_VALUE && ::ReadFile(in, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
            buffer.append(chunk, n);
            for (auto eol = buffer.find('\n'); eol != std::string::npos; eol = buffer.find('\n')) {
                std::string line = buffer.substr(0, eol);
                buffer.erase(0, eol + 1);
                // Tolerate what scripts send: a UTF-8 byte-order mark (.NET writes one when
                // the console encoding is UTF-8) and surrounding whitespace, including CR.
                if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
                if (line.rfind("metrics ", 0) == 0) {
                    write_metrics(engine, line.substr(8));
                    std::printf("metrics written: %s\n", line.substr(8).c_str());
                    std::fflush(stdout);
                } else if (line == "reload") {  // the same path the file watcher takes
                    const auto r = engine.reload_from_file(config_path);
                    if (r.accepted) std::printf("reload applied: %s\n", r.summary.c_str());
                    else if (r.unchanged) std::printf("reload skipped: %s\n", r.summary.c_str());
                    else std::printf("reload rejected: %s\n", r.errors.front().c_str());
                    std::fflush(stdout);
                } else if (line == "quit") {
                    ::SetEvent(g_stop_event);
                    return;
                }
            }
        }
    });
    commands.detach();  // may stay blocked on stdin; the process exits after stop

    ::WaitForSingleObject(g_stop_event, INFINITE);

    std::printf("stopping...\n");
    engine.stop();
    const auto s = engine.stats();
    std::printf("stopped: %llu connections, %llu requests, %llu error responses, %llu backend connections opened, "
                "%llu requests on reused connections\n",
                static_cast<unsigned long long>(s.connections_accepted),
                static_cast<unsigned long long>(s.requests_completed),
                static_cast<unsigned long long>(s.error_responses),
                static_cast<unsigned long long>(s.backend_connections_opened),
                static_cast<unsigned long long>(s.backend_connections_reused));
    std::fflush(stdout);
    ::CloseHandle(g_stop_event);
    return 0;
}
