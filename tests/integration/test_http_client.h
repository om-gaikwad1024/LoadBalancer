#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace lbtest {

// Minimal blocking HTTP/1.1 client for integration tests, independent of the engine's
// parser. It reads Content-Length, chunked and close-delimited bodies and reports how a
// response ended, so tests can tell a complete response from a FIN, an RST or a timeout.
struct ClientResponse {
    enum class End { Complete, Closed, Reset, Timeout, Error };
    enum class Framing { None, ContentLength, Chunked, UntilClose };
    End end = End::Error;
    Framing framing = Framing::None;
    int status = 0;
    std::string head;
    std::map<std::string, std::string> headers;  // lowercase names
    std::string body;
    std::size_t content_length = 0;

    std::string header(const std::string& lowercase_name) const {
        const auto it = headers.find(lowercase_name);
        return it == headers.end() ? std::string() : it->second;
    }
};

class TestClient {
public:
    explicit TestClient(std::uint16_t port, DWORD recv_timeout_ms = 5000) {
        WSADATA wsa{};
        wsa_ok_ = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout_ms),
                     sizeof(recv_timeout_ms));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connected_ = ::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    ~TestClient() {
        if (socket_ != INVALID_SOCKET) ::closesocket(socket_);
        if (wsa_ok_) ::WSACleanup();
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    bool connected() const noexcept { return connected_; }

    bool send(std::string_view data) {
        while (!data.empty()) {
            const int n = ::send(socket_, data.data(), static_cast<int>(data.size()), 0);
            if (n == SOCKET_ERROR) return false;
            data.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }

    ClientResponse read_response(bool head_request = false) {
        ClientResponse r;
        std::size_t end;
        while ((end = buffer_.find("\r\n\r\n")) == std::string::npos) {
            if (!recv_more(&r.end)) return r;
        }
        r.head = buffer_.substr(0, end + 4);
        buffer_.erase(0, end + 4);

        const std::string_view head(r.head);
        if (head.size() >= 12) std::from_chars(head.data() + 9, head.data() + 12, r.status);
        for (std::size_t p = head.find("\r\n") + 2; p + 2 < head.size();) {
            const std::size_t eol = head.find("\r\n", p);
            const std::string_view line = head.substr(p, eol - p);
            p = eol + 2;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string name(line.substr(0, colon));
            std::transform(name.begin(), name.end(), name.begin(), [](char c) {
                return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            });
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            r.headers[name] = std::string(value);
        }
        const std::string cl = r.header("content-length");
        std::from_chars(cl.data(), cl.data() + cl.size(), r.content_length);

        const bool no_body = head_request || r.status < 200 || r.status == 204 || r.status == 304;
        if (no_body) {
            r.end = ClientResponse::End::Complete;
            return r;
        }
        if (r.header("transfer-encoding").find("chunked") != std::string::npos) {
            r.framing = ClientResponse::Framing::Chunked;
            return read_chunked(std::move(r));
        }
        if (cl.empty()) {  // close-delimited: the body ends when the server closes
            r.framing = ClientResponse::Framing::UntilClose;
            while (recv_more(&r.end)) {
            }
            r.body = std::move(buffer_);
            buffer_.clear();
            return r;
        }

        r.framing = ClientResponse::Framing::ContentLength;
        const std::size_t want = r.content_length;
        while (buffer_.size() < want) {
            if (!recv_more(&r.end)) {
                r.body = buffer_;
                buffer_.clear();
                return r;
            }
        }
        r.body = buffer_.substr(0, want);
        buffer_.erase(0, want);
        r.end = ClientResponse::End::Complete;
        return r;
    }

    // Waits for the server's next move after a response: Closed (FIN), Reset, or Timeout.
    ClientResponse::End wait_for_close() {
        ClientResponse::End end = ClientResponse::End::Error;
        while (recv_more(&end)) {
        }
        return end;
    }

private:
    bool read_line(std::string* line, ClientResponse::End* end) {
        std::size_t eol;
        while ((eol = buffer_.find("\r\n")) == std::string::npos) {
            if (!recv_more(end)) return false;
        }
        *line = buffer_.substr(0, eol);
        buffer_.erase(0, eol + 2);
        return true;
    }

    ClientResponse read_chunked(ClientResponse r) {
        std::string line;
        for (;;) {
            if (!read_line(&line, &r.end)) return r;
            std::size_t size = 0;
            std::from_chars(line.data(), line.data() + line.size(), size, 16);
            if (size == 0) {
                do {
                    if (!read_line(&line, &r.end)) return r;
                } while (!line.empty());
                r.end = ClientResponse::End::Complete;
                return r;
            }
            while (buffer_.size() < size + 2) {
                if (!recv_more(&r.end)) {
                    r.body += buffer_;
                    buffer_.clear();
                    return r;
                }
            }
            r.body += buffer_.substr(0, size);
            buffer_.erase(0, size + 2);
        }
    }

    bool recv_more(ClientResponse::End* end) {
        char tmp[8192];
        const int n = ::recv(socket_, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (n > 0) {
            buffer_.append(tmp, static_cast<std::size_t>(n));
            return true;
        }
        if (n == 0) {
            *end = ClientResponse::End::Closed;
        } else {
            const int err = ::WSAGetLastError();
            *end = err == WSAECONNRESET || err == WSAECONNABORTED ? ClientResponse::End::Reset
                   : err == WSAETIMEDOUT                         ? ClientResponse::End::Timeout
                                                                 : ClientResponse::End::Error;
        }
        return false;
    }

    bool wsa_ok_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    bool connected_ = false;
    std::string buffer_;
};

inline std::string get_request(std::string_view target, std::string_view extra_headers = {}) {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: test\r\n" + std::string(extra_headers) + "\r\n";
}

// One request on a fresh connection.
inline ClientResponse fetch(std::uint16_t port, std::string_view target, std::string_view extra_headers = {}) {
    TestClient c(port);
    if (!c.connected() || !c.send(get_request(target, extra_headers))) return {};
    return c.read_response();
}

}  // namespace lbtest
