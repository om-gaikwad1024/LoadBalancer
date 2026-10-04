#pragma once

#include <winsock2.h>
#include <mswsock.h>

#include <string>

namespace lb::net {

// WSAStartup/WSACleanup pair owned by the engine.
class WinsockRuntime {
public:
    WinsockRuntime() = default;
    ~WinsockRuntime();
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;

    bool init(std::string* error);

private:
    bool started_ = false;
};

// Winsock extension functions, loaded once through WSAIoctl.
struct SocketExtensions {
    LPFN_ACCEPTEX accept_ex = nullptr;
    LPFN_CONNECTEX connect_ex = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS get_accept_ex_sockaddrs = nullptr;

    bool load(std::string* error);
};

// TCP socket usable with overlapped I/O (IOCP).
SOCKET make_overlapped_tcp_socket() noexcept;

void set_no_delay(SOCKET s) noexcept;

std::string wsa_error_text(int code);

}  // namespace lb::net
