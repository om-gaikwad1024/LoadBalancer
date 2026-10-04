#include "mock_server.h"

#include <windows.h>
#include <timeapi.h>

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

HANDLE g_stop_event = nullptr;

BOOL WINAPI on_console_ctrl(DWORD) {
    ::SetEvent(g_stop_event);
    return TRUE;
}

int usage(const std::string& problem) {
    if (!problem.empty()) std::fprintf(stderr, "mock_backend: %s\n", problem.c_str());
    std::fprintf(stderr,
                 "usage: mock_backend --port <0-65535> [options]\n"
                 "  --bind <ipv4>             listen address (default 127.0.0.1)\n"
                 "  --id <name>               X-Backend-Id header (default: the port)\n"
                 "  --seed <n>                fault decision seed (default 1)\n"
                 "  --max-connections <n>     concurrent connection cap (default 1024)\n"
                 "  --health-path <path>      health endpoint (default /health)\n"
                 "fault switches (also settable at runtime: GET /__mock/set?key=value&...):\n"
                 "  --latency-ms <n>  --error-rate <0..1>  --error-status <code>  --close-rate <0..1>\n"
                 "  --partial-rate <0..1>  --echo-headers <0|1>  --health-status <code>  --body-bytes <n>\n"
                 "control: /__mock/set  /__mock/faults  /__mock/stats  /__mock/reset\n");
    return 2;
}

template <typename T>
bool parse_uint(std::string_view text, T* out) {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

}  // namespace

int main(int argc, char** argv) {
    mock::MockOptions options;
    bool have_port = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.substr(0, 2) != "--" || i + 1 >= argc) return usage("bad argument: " + std::string(arg));
        const std::string_view value = argv[++i];
        if (arg == "--port") {
            if (!parse_uint(value, &options.port)) return usage("invalid --port");
            have_port = true;
        } else if (arg == "--bind") {
            options.bind_address = std::string(value);
        } else if (arg == "--id") {
            options.id = std::string(value);
        } else if (arg == "--seed") {
            if (!parse_uint(value, &options.seed)) return usage("invalid --seed");
        } else if (arg == "--max-connections") {
            if (!parse_uint(value, &options.max_connections) || options.max_connections == 0) {
                return usage("invalid --max-connections");
            }
        } else if (arg == "--health-path") {
            options.health_path = std::string(value);
        } else {
            // --latency-ms -> latency_ms, etc.
            std::string key(arg.substr(2));
            for (char& c : key) {
                if (c == '-') c = '_';
            }
            const std::string error = mock::apply_fault_setting(options.faults, key, value);
            if (!error.empty()) return usage(error);
        }
    }
    if (!have_port) return usage("--port is required");

    // Latency injection sleeps; with the default ~15.6 ms timer tick a 2 ms sleep takes ~10 ms.
    // A 1 ms tick makes --latency-ms accurate (this is a test tool; the proxy never sleeps).
    ::timeBeginPeriod(1);

    mock::MockServer server(options);
    std::string error;
    if (!server.start(&error)) {
        std::fprintf(stderr, "mock_backend: %s\n", error.c_str());
        return 1;
    }
    std::printf("mock_backend listening on %s:%u id=%s faults=%s\n", options.bind_address.c_str(), server.port(),
                server.id().c_str(), mock::faults_to_json(server.faults()).c_str());
    std::fflush(stdout);

    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    ::WaitForSingleObject(g_stop_event, INFINITE);

    server.stop();
    ::CloseHandle(g_stop_event);
    ::timeEndPeriod(1);
    return 0;
}
