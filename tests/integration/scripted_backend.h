#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lbtest {

// A backend that answers every request with fixed raw bytes, for response shapes the
// mock backend never produces (chunked, close-delimited, interim 1xx, garbage, partial).
// Serves one connection at a time on a blocking thread.
class ScriptedBackend {
public:
    // close_after: close the connection after each response (also ends close-delimited bodies).
    explicit ScriptedBackend(std::string response, bool close_after = true)
        : response_(std::move(response)), close_after_(close_after) {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listener_, 16);
        int len = sizeof(addr);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~ScriptedBackend() {
        stopping_ = true;
        ::closesocket(listener_);
        if (thread_.joinable()) thread_.join();
        ::WSACleanup();
    }

    ScriptedBackend(const ScriptedBackend&) = delete;
    ScriptedBackend& operator=(const ScriptedBackend&) = delete;

    std::uint16_t port() const noexcept { return port_; }

    std::vector<std::string> requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (!stopping_) {
            SOCKET s = ::accept(listener_, nullptr, nullptr);
            if (s == INVALID_SOCKET) return;
            const DWORD timeout_ms = 2000;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            serve(s);
            ::closesocket(s);
        }
    }

    void serve(SOCKET s) {
        std::string buf;
        while (!stopping_) {
            std::size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                if (!recv_more(s, &buf)) return;
            }
            std::string request = buf.substr(0, end + 4);
            buf.erase(0, end + 4);
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(request);
            }
            ::send(s, response_.data(), static_cast<int>(response_.size()), 0);
            if (close_after_) {
                ::shutdown(s, SD_SEND);
                while (recv_more(s, &buf)) {  // wait for the proxy to close its side
                }
                return;
            }
        }
    }

    static bool recv_more(SOCKET s, std::string* buf) {
        char tmp[4096];
        const int n = ::recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (n <= 0) return false;
        buf->append(tmp, static_cast<std::size_t>(n));
        return true;
    }

    std::string response_;
    bool close_after_;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

}  // namespace lbtest
