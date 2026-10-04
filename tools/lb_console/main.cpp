// Headless host for the proxy engine: same engine as the MFC app, without a GUI.
// Used for load tests, soak runs and scripted scenarios.
//   lb_console --config config\lb.example.json

#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "config/config_loader.h"
#include "engine.h"

namespace {

HANDLE g_stop_event = nullptr;

BOOL WINAPI on_console_ctrl(DWORD) {
    ::SetEvent(g_stop_event);
    return TRUE;
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
    std::fflush(stdout);

    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(on_console_ctrl, TRUE);
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
    ::CloseHandle(g_stop_event);
    return 0;
}
