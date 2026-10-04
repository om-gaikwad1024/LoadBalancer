#include "mock_server.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace mock {

namespace {

constexpr std::size_t kMaxHeadBytes = 64 * 1024;
constexpr std::size_t kRecvChunk = 16 * 1024;
constexpr std::string_view kControlPrefix = "/__mock/";

std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool list_has_token(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        if (to_lower(trim(value.substr(0, comma))) == token) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

const char* reason_phrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 418: return "I'm a teapot";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

bool send_all(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int sent = ::send(s, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 1 << 30)), 0);
        if (sent == SOCKET_ERROR) return false;
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

std::string response_head(int status, std::size_t content_length, std::string_view backend_id, bool keep_alive) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
    head += "Content-Type: text/plain\r\n";
    head += "Content-Length: " + std::to_string(content_length) + "\r\n";
    head += "X-Backend-Id: " + std::string(backend_id) + "\r\n";
    if (!keep_alive) head += "Connection: close\r\n";
    head += "\r\n";
    return head;
}

template <typename T>
bool parse_number(std::string_view text, T* out) {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::string default_body(std::uint32_t bytes) {
    std::string body = "ok";
    body.resize(bytes, '.');
    return body;
}

// Reads from one connection, buffering bytes beyond the current request.
class Reader {
public:
    explicit Reader(SOCKET s) : socket_(s) {}

    std::string buffer;

    bool recv_more() {
        char tmp[kRecvChunk];
        const int n = ::recv(socket_, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (n <= 0) return false;
        buffer.append(tmp, static_cast<std::size_t>(n));
        return true;
    }

    // Takes one CRLF-terminated line (without CRLF).
    bool take_line(std::string* line) {
        std::size_t eol;
        while ((eol = buffer.find("\r\n")) == std::string::npos) {
            if (buffer.size() > kMaxHeadBytes || !recv_more()) return false;
        }
        line->assign(buffer, 0, eol);
        buffer.erase(0, eol + 2);
        return true;
    }

    // Consumes n bytes, appending them to *sink when it is not null.
    bool take(std::uint64_t n, std::string* sink) {
        while (buffer.size() < n) {
            n -= buffer.size();
            if (sink != nullptr) sink->append(buffer);
            buffer.clear();
            if (!recv_more()) return false;
        }
        if (sink != nullptr) sink->append(buffer, 0, static_cast<std::size_t>(n));
        buffer.erase(0, static_cast<std::size_t>(n));
        return true;
    }

private:
    SOCKET socket_;
};

}  // namespace

std::string apply_fault_setting(MockFaults& f, std::string_view key, std::string_view value) {
    const auto rate = [&](double* field) -> std::string {
        double v = 0;
        if (!parse_number(value, &v) || v < 0.0 || v > 1.0) return std::string(key) + " must be a number in [0, 1]";
        *field = v;
        return {};
    };
    const auto status = [&](int* field) -> std::string {
        int v = 0;
        if (!parse_number(value, &v) || v < 100 || v > 599) return std::string(key) + " must be an HTTP status 100-599";
        *field = v;
        return {};
    };
    const auto count = [&](std::uint32_t* field, std::uint32_t max) -> std::string {
        std::uint32_t v = 0;
        if (!parse_number(value, &v) || v > max) return std::string(key) + " must be an integer 0-" + std::to_string(max);
        *field = v;
        return {};
    };

    if (key == "latency_ms") return count(&f.latency_ms, 600'000);
    if (key == "health_latency_ms") return count(&f.health_latency_ms, 600'000);
    if (key == "error_rate") return rate(&f.error_rate);
    if (key == "error_status") return status(&f.error_status);
    if (key == "close_rate") return rate(&f.close_rate);
    if (key == "partial_rate") return rate(&f.partial_rate);
    if (key == "health_status") return status(&f.health_status);
    if (key == "body_bytes") return count(&f.body_bytes, 64u * 1024 * 1024);
    const auto flag = [&](bool* field) -> std::string {
        if (value == "1" || value == "true") *field = true;
        else if (value == "0" || value == "false") *field = false;
        else return std::string(key) + " must be 0, 1, true or false";
        return {};
    };
    if (key == "echo_headers") return flag(&f.echo_headers);
    if (key == "echo_body") return flag(&f.echo_body);
    return "unknown setting: " + std::string(key);
}

std::string faults_to_json(const MockFaults& f) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"latency_ms\":%u,\"error_rate\":%g,\"error_status\":%d,\"close_rate\":%g,\"partial_rate\":%g,"
                  "\"echo_headers\":%s,\"echo_body\":%s,\"health_status\":%d,\"health_latency_ms\":%u,"
                  "\"body_bytes\":%u}",
                  f.latency_ms, f.error_rate, f.error_status, f.close_rate, f.partial_rate,
                  f.echo_headers ? "true" : "false", f.echo_body ? "true" : "false", f.health_status,
                  f.health_latency_ms, f.body_bytes);
    return buf;
}

MockServer::MockServer(MockOptions options) : options_(std::move(options)), faults_(options_.faults) {}

MockServer::~MockServer() { stop(); }

bool MockServer::start(std::string* error) {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        *error = "WSAStartup failed";
        return false;
    }
    wsa_started_ = true;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
        *error = "invalid bind address: " + options_.bind_address;
        return false;
    }

    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
        *error = "socket() failed: " + std::to_string(::WSAGetLastError());
        return false;
    }
    // A restarted mock must be able to bind the port again immediately (kill/restart tests),
    // but never share it with another live listener.
    BOOL exclusive = TRUE;
    ::setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(listener_, SOMAXCONN) == SOCKET_ERROR) {
        *error = "bind/listen failed: " + std::to_string(::WSAGetLastError());
        ::closesocket(listener_);
        listener_ = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound{};
    int len = sizeof(bound);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &len);
    bound_port_ = ::ntohs(bound.sin_port);
    id_ = options_.id.empty() ? std::to_string(bound_port_) : options_.id;

    accept_thread_ = std::thread([this, listener = listener_] { accept_loop(listener); });
    return true;
}

void MockServer::stop() {
    if (stopping_.exchange(true)) return;
    {
        std::lock_guard lock(stop_mutex_);
    }
    stop_cv_.notify_all();  // wakes requests sleeping in latency mode

    if (listener_ != INVALID_SOCKET) {
        ::closesocket(listener_);  // unblocks accept()
        listener_ = INVALID_SOCKET;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    std::list<std::unique_ptr<Connection>> remaining;
    {
        std::lock_guard lock(connections_mutex_);
        remaining.swap(connections_);
    }
    for (auto& c : remaining) {
        std::lock_guard lock(c->socket_mutex);
        if (c->socket != INVALID_SOCKET) ::shutdown(c->socket, SD_BOTH);  // unblocks recv()
    }
    for (auto& c : remaining) {
        if (c->thread.joinable()) c->thread.join();
    }

    if (wsa_started_) {
        ::WSACleanup();
        wsa_started_ = false;
    }
}

MockFaults MockServer::faults() const {
    std::lock_guard lock(faults_mutex_);
    return faults_;
}

void MockServer::set_faults(const MockFaults& faults) {
    std::lock_guard lock(faults_mutex_);
    faults_ = faults;
}

MockStats MockServer::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void MockServer::reset_stats() {
    std::lock_guard lock(stats_mutex_);
    const auto active = stats_.active_connections;
    stats_ = {};
    stats_.active_connections = active;
}

std::string MockServer::stats_json() const {
    const MockStats s = stats();
    std::string json = "{\"id\":\"" + id_ + "\",\"requests\":" + std::to_string(s.requests) +
                       ",\"health_requests\":" + std::to_string(s.health_requests) +
                       ",\"connections\":" + std::to_string(s.connections) +
                       ",\"active_connections\":" + std::to_string(s.active_connections) +
                       ",\"refused_connections\":" + std::to_string(s.refused_connections) +
                       ",\"aborted\":" + std::to_string(s.aborted) + ",\"partial\":" + std::to_string(s.partial) +
                       ",\"status\":{";
    bool first = true;
    for (const auto& [status, n] : s.status_counts) {
        json += (first ? "\"" : ",\"") + std::to_string(status) + "\":" + std::to_string(n);
        first = false;
    }
    return json + "}}";
}

void MockServer::count_status(int status) {
    std::lock_guard lock(stats_mutex_);
    ++stats_.status_counts[status];
}

double MockServer::draw(std::uint64_t request_number, int which) const noexcept {
    const std::uint64_t x = splitmix64(options_.seed ^ splitmix64(request_number * 4 + static_cast<std::uint64_t>(which)));
    return static_cast<double>(x >> 11) * 0x1.0p-53;
}

bool MockServer::sleep_unless_stopping(std::uint32_t ms) {
    std::unique_lock lock(stop_mutex_);
    return !stop_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping_.load(); });
}

void MockServer::accept_loop(SOCKET listener) {
    while (!stopping_) {
        SOCKET s = ::accept(listener, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (stopping_) return;
            continue;
        }
        bool over_cap = false;
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.connections;
            over_cap = stats_.active_connections >= options_.max_connections;
            if (over_cap) ++stats_.refused_connections;
            else ++stats_.active_connections;
        }
        if (over_cap) {
            send_all(s, response_head(503, 0, id_, false));
            ::shutdown(s, SD_SEND);
            ::closesocket(s);
            continue;
        }

        std::lock_guard lock(connections_mutex_);
        reap_finished_locked();
        auto conn = std::make_unique<Connection>();
        conn->socket = s;
        Connection* raw = conn.get();
        connections_.push_back(std::move(conn));
        raw->thread = std::thread([this, raw] { serve(raw); });
    }
}

void MockServer::reap_finished_locked() {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if ((*it)->done) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void MockServer::serve(Connection* conn) {
    const SOCKET s = conn->socket;
    Reader reader(s);
    Outcome outcome = Outcome::Close;

    while (!stopping_) {
        // Head: everything up to the empty line.
        std::size_t end;
        bool have_head = true;
        while ((end = reader.buffer.find("\r\n\r\n")) == std::string::npos) {
            if (reader.buffer.size() > kMaxHeadBytes) {
                send_all(s, response_head(431, 0, id_, false));
                have_head = false;
                break;
            }
            if (!reader.recv_more()) {
                have_head = false;
                break;
            }
        }
        if (!have_head) break;
        const std::string head = reader.buffer.substr(0, end + 4);
        reader.buffer.erase(0, end + 4);

        // Request line: METHOD SP TARGET SP VERSION
        const std::string_view hv(head);
        const std::string_view request_line = hv.substr(0, hv.find("\r\n"));
        const auto sp1 = request_line.find(' ');
        const auto sp2 = sp1 == std::string_view::npos ? sp1 : request_line.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos || sp1 == 0 || sp2 == sp1 + 1) {
            send_all(s, response_head(400, 0, id_, false));
            break;
        }
        const std::string_view method = request_line.substr(0, sp1);
        const std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string_view version = request_line.substr(sp2 + 1);

        // Headers needed for framing and keep-alive only.
        std::uint64_t content_length = 0;
        bool chunked = false;
        bool conn_close = false;
        bool conn_keep_alive = false;
        bool malformed = version != "HTTP/1.1" && version != "HTTP/1.0";
        for (std::size_t p = request_line.size() + 2; p + 2 < hv.size();) {
            const std::size_t eol = hv.find("\r\n", p);
            const std::string_view line = hv.substr(p, eol - p);
            p = eol + 2;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                malformed = true;
                break;
            }
            const std::string name = to_lower(trim(line.substr(0, colon)));
            const std::string_view value = trim(line.substr(colon + 1));
            if (name == "content-length" && !parse_number(value, &content_length)) malformed = true;
            if (name == "transfer-encoding") chunked = list_has_token(value, "chunked");
            if (name == "connection") {
                conn_close = conn_close || list_has_token(value, "close");
                conn_keep_alive = conn_keep_alive || list_has_token(value, "keep-alive");
            }
        }
        if (malformed) {
            send_all(s, response_head(400, 0, id_, false));
            break;
        }

        // Body: read and discard, or keep it for echo_body.
        std::string body;
        std::string* sink = faults().echo_body ? &body : nullptr;
        bool body_ok = true;
        if (chunked) {
            std::string line;
            for (;;) {
                if (!reader.take_line(&line)) {
                    body_ok = false;
                    break;
                }
                std::uint64_t size = 0;
                const std::string_view size_text = std::string_view(line).substr(0, line.find(';'));
                const auto [ptr, ec] = std::from_chars(size_text.data(), size_text.data() + size_text.size(), size, 16);
                if (ec != std::errc{} || size_text.empty()) {
                    body_ok = false;
                    break;
                }
                if (size == 0) {  // trailers, up to the empty line
                    do {
                        body_ok = reader.take_line(&line);
                    } while (body_ok && !line.empty());
                    break;
                }
                if (!reader.take(size, sink) || !reader.take(2, nullptr)) {
                    body_ok = false;
                    break;
                }
            }
        } else if (content_length > 0) {
            body_ok = reader.take(content_length, sink);
        }
        if (!body_ok) break;

        const bool keep_alive = version == "HTTP/1.1" ? !conn_close : conn_keep_alive;
        outcome = handle_request(s, method, target, head, body, keep_alive);
        if (outcome != Outcome::KeepOpen) break;
    }

    {
        std::lock_guard lock(conn->socket_mutex);
        if (outcome == Outcome::Abort) {
            linger lg{1, 0};  // closesocket() then sends RST instead of FIN
            ::setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        } else {
            ::shutdown(s, SD_SEND);
        }
        ::closesocket(s);
        conn->socket = INVALID_SOCKET;
    }
    {
        std::lock_guard lock(stats_mutex_);
        --stats_.active_connections;
    }
    conn->done = true;
}

MockServer::Outcome MockServer::handle_request(SOCKET s, std::string_view method, std::string_view target,
                                               std::string_view raw_head, std::string_view request_body,
                                               bool keep_alive) {
    const auto qmark = target.find('?');
    const std::string_view path = target.substr(0, qmark);
    std::string_view query = qmark == std::string_view::npos ? std::string_view{} : target.substr(qmark + 1);
    const bool is_head = method == "HEAD";
    const Outcome after = keep_alive ? Outcome::KeepOpen : Outcome::Close;

    const auto respond = [&](int status, std::string_view body) {
        const std::string head = response_head(status, body.size(), id_, keep_alive);
        const bool ok = send_all(s, head) && (is_head || send_all(s, body));
        return ok ? after : Outcome::Close;
    };

    // Control endpoints: change switches and read counters from scripts (plan IX chaos tests).
    if (path.substr(0, kControlPrefix.size()) == kControlPrefix) {
        const std::string_view action = path.substr(kControlPrefix.size());
        if (action == "set") {
            MockFaults next = faults();
            while (!query.empty()) {
                const auto amp = query.find('&');
                const std::string_view pair = query.substr(0, amp);
                const auto eq = pair.find('=');
                const std::string err = eq == std::string_view::npos
                                            ? "expected key=value"
                                            : apply_fault_setting(next, pair.substr(0, eq), pair.substr(eq + 1));
                if (!err.empty()) return respond(400, err + "\n");
                query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
            }
            set_faults(next);
            return respond(200, faults_to_json(next) + "\n");
        }
        if (action == "faults") return respond(200, faults_to_json(faults()) + "\n");
        if (action == "stats") return respond(200, stats_json() + "\n");
        if (action == "reset") {
            reset_stats();
            return respond(200, "reset\n");
        }
        return respond(404, "unknown control endpoint\n");
    }

    const MockFaults f = faults();

    if (path == options_.health_path) {
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.health_requests;
        }
        if (f.health_latency_ms > 0 && !sleep_unless_stopping(f.health_latency_ms)) return Outcome::Abort;
        return respond(f.health_status, f.health_status < 400 ? "healthy\n" : "unhealthy\n");
    }

    const std::uint64_t n = request_counter_.fetch_add(1);
    {
        std::lock_guard lock(stats_mutex_);
        ++stats_.requests;
    }

    if (f.latency_ms > 0 && !sleep_unless_stopping(f.latency_ms)) return Outcome::Abort;

    if (draw(n, 0) < f.close_rate) {
        std::lock_guard lock(stats_mutex_);
        ++stats_.aborted;
        return Outcome::Abort;
    }

    std::string body;
    if (f.echo_headers || f.echo_body) {
        if (f.echo_headers) body.append(raw_head);
        if (f.echo_body) body.append(request_body);
    } else {
        body = default_body(f.body_bytes);
    }

    if (draw(n, 1) < f.partial_rate) {
        if (body.size() < 2) body = default_body(2);
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.partial;
        }
        // Promise the whole body, send half, then close: the client must see an incomplete response.
        send_all(s, response_head(200, body.size(), id_, true));
        send_all(s, std::string_view(body).substr(0, body.size() / 2));
        return Outcome::Close;
    }

    if (draw(n, 2) < f.error_rate) {
        count_status(f.error_status);
        return respond(f.error_status, "mock error\n");
    }

    count_status(200);
    return respond(200, body);
}

}  // namespace mock
