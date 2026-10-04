#include "net/winsock.h"

#include <ws2tcpip.h>

namespace lb::net {

namespace {

template <typename Fn>
bool load_extension(SOCKET s, GUID guid, Fn* out) {
    DWORD bytes = 0;
    return ::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), out, sizeof(*out), &bytes, nullptr,
                      nullptr) == 0;
}

}  // namespace

WinsockRuntime::~WinsockRuntime() {
    if (started_) ::WSACleanup();
}

bool WinsockRuntime::init(std::string* error) {
    WSADATA wsa{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        *error = "WSAStartup failed: " + wsa_error_text(rc);
        return false;
    }
    started_ = true;
    return true;
}

bool SocketExtensions::load(std::string* error) {
    const SOCKET s = make_overlapped_tcp_socket();
    if (s == INVALID_SOCKET) {
        *error = "socket() failed: " + wsa_error_text(::WSAGetLastError());
        return false;
    }
    const bool ok = load_extension(s, WSAID_ACCEPTEX, &accept_ex) && load_extension(s, WSAID_CONNECTEX, &connect_ex) &&
                    load_extension(s, WSAID_GETACCEPTEXSOCKADDRS, &get_accept_ex_sockaddrs);
    if (!ok) *error = "loading Winsock extension functions failed: " + wsa_error_text(::WSAGetLastError());
    ::closesocket(s);
    return ok;
}

SOCKET make_overlapped_tcp_socket() noexcept {
    return ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
}

void set_no_delay(SOCKET s) noexcept {
    BOOL on = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

std::string wsa_error_text(int code) {
    char* text = nullptr;
    const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string out = std::to_string(code);
    if (n > 0 && text != nullptr) {
        std::string msg(text, n);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
        out += " (" + msg + ")";
    }
    if (text != nullptr) ::LocalFree(text);
    return out;
}

}  // namespace lb::net
