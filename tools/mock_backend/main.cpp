#include "mock_server.h"

#include <windows.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

HANDLE g_stop_event = nullptr;

BOOL WINAPI on_console_ctrl(DWORD) {
    ::SetEvent(g_stop_event);
    return TRUE;
}

int usage() {
    std::fprintf(stderr, "usage: mock_backend --port <0-65535> [--bind <ipv4>]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    mock::MockOptions options;
    options.bind_address = "127.0.0.1";
    bool have_port = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) return usage();
        const std::string_view value = argv[++i];
        if (arg == "--port") {
            unsigned port = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (ec != std::errc{} || ptr != value.data() + value.size() || port > 65535) return usage();
            options.port = static_cast<std::uint16_t>(port);
            have_port = true;
        } else if (arg == "--bind") {
            options.bind_address = std::string(value);
        } else {
            return usage();
        }
    }
    if (!have_port) return usage();

    mock::MockServer server(options);
    std::string error;
    if (!server.start(&error)) {
        std::fprintf(stderr, "mock_backend: %s\n", error.c_str());
        return 1;
    }
    std::printf("mock_backend listening on %s:%u\n", options.bind_address.c_str(), server.port());
    std::fflush(stdout);

    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    ::WaitForSingleObject(g_stop_event, INFINITE);

    server.stop();
    ::CloseHandle(g_stop_event);
    return 0;
}
