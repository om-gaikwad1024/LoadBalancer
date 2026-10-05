#include "health/health_checker.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

#include "net/winsock.h"

namespace lb::health {

namespace {

constexpr std::size_t kMaxStatusLineBytes = 1024;  // enough for any status line; more is a broken probe

bool set_non_blocking(SOCKET s) noexcept {
    u_long on = 1;
    return ::ioctlsocket(s, FIONBIO, &on) == 0;
}


// "HTTP/1.x NNN ..." -> NNN, or 0 if the line is not a status line.
int parse_status(std::string_view line) noexcept {
    if (line.size() < 12 || line.substr(0, 7) != "HTTP/1." || line[8] != ' ') return 0;
    int status = 0;
    for (std::size_t i = 9; i < 12; ++i) {
        if (line[i] < '0' || line[i] > '9') return 0;
        status = status * 10 + (line[i] - '0');
    }
    return status;
}

}  // namespace

bool HealthChecker::start(const ConfigSnapshot& config,
                          const std::vector<std::shared_ptr<backend::BackendRuntime>>& backends, Listener listener,
                          std::string* error) {
    listener_ = std::move(listener);

    wake_socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    wake_address_.sin_family = AF_INET;
    ::inet_pton(AF_INET, "127.0.0.1", &wake_address_.sin_addr);
    int len = sizeof(wake_address_);
    if (wake_socket_ == INVALID_SOCKET ||
        ::bind(wake_socket_, reinterpret_cast<sockaddr*>(&wake_address_), sizeof(wake_address_)) == SOCKET_ERROR ||
        ::getsockname(wake_socket_, reinterpret_cast<sockaddr*>(&wake_address_), &len) == SOCKET_ERROR) {
        *error = "health checker wake socket failed: " + net::wsa_error_text(::WSAGetLastError());
        return false;
    }

    const TimePoint now = Clock::now();
    std::vector<std::size_t> per_group_index(config.groups.size(), 0);
    for (const auto& b : backends) {
        const auto group_it = std::find_if(config.groups.begin(), config.groups.end(),
                                           [&](const GroupConfig& g) { return g.name == b->group; });
        if (group_it == config.groups.end()) continue;
        const HealthConfig& hc = group_it->health;
        const auto members = group_it->backends.size();
        const auto index = per_group_index[static_cast<std::size_t>(group_it - config.groups.begin())]++;

        Probe p{b, hc, {}, Hysteresis(hc.unhealthy_threshold, hc.healthy_threshold),
                b->times_marked_down.load(std::memory_order_acquire)};
        if (hc.type == HealthConfig::Type::Http) {
            p.request = "GET " + hc.path + " HTTP/1.1\r\nHost: " + b->endpoint +
                        "\r\nUser-Agent: lb-health-check\r\nConnection: close\r\n\r\n";
        }
        // Stagger: backend i of n in a group starts at (i+1)/n of the interval.
        const auto offset = std::chrono::milliseconds(hc.interval_ms) * static_cast<long long>(index + 1) /
                            static_cast<long long>(members);
        p.next_start = now + offset;
        probes_.push_back(std::move(p));
    }

    thread_ = std::thread([this] {
        ::SetThreadDescription(::GetCurrentThread(), L"lb-health-check");
        run();
    });
    return true;
}

void HealthChecker::stop() {
    if (stopping_.exchange(true)) return;
    wake();
    if (thread_.joinable()) thread_.join();
    for (auto& p : probes_) {
        if (p.socket != INVALID_SOCKET) ::closesocket(p.socket);
        p.socket = INVALID_SOCKET;
    }
    if (wake_socket_ != INVALID_SOCKET) ::closesocket(wake_socket_);
    wake_socket_ = INVALID_SOCKET;
}

void HealthChecker::wake() noexcept {
    if (wake_socket_ == INVALID_SOCKET) return;
    const char byte = 1;
    ::sendto(wake_socket_, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&wake_address_), sizeof(wake_address_));
}

void HealthChecker::run() {
    std::vector<WSAPOLLFD> fds;
    std::vector<Probe*> owners;
    while (!stopping_) {
        TimePoint now = Clock::now();
        TimePoint next_event = now + std::chrono::seconds(60);
        for (auto& p : probes_) {
            if (p.phase == Phase::Idle && now >= p.next_start) begin(p, now);
            if (p.phase != Phase::Idle && now >= p.deadline) finish(p, false, "timeout");
            next_event = std::min(next_event, p.phase == Phase::Idle ? p.next_start : p.deadline);
        }

        fds.clear();
        owners.clear();
        fds.push_back({wake_socket_, POLLRDNORM, 0});
        owners.push_back(nullptr);
        for (auto& p : probes_) {
            if (p.phase == Phase::Idle) continue;
            fds.push_back({p.socket, static_cast<short>(p.phase == Phase::Receiving ? POLLRDNORM : POLLWRNORM), 0});
            owners.push_back(&p);
        }

        now = Clock::now();
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_event - now).count();
        const int timeout_ms = static_cast<int>(std::clamp<long long>(wait, 0, 60'000));
        if (::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms) == SOCKET_ERROR) continue;

        if (fds[0].revents != 0) {  // woken by stop()
            char drain[16];
            ::recv(wake_socket_, drain, sizeof(drain), 0);
            continue;
        }
        for (std::size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents != 0) advance(*owners[i], fds[i].revents);
        }
    }
}

void HealthChecker::begin(Probe& p, TimePoint now) {
    p.next_start = now + std::chrono::milliseconds(p.config.interval_ms);
    p.deadline = now + std::chrono::milliseconds(p.config.timeout_ms);
    p.sent = 0;
    p.received.clear();

    p.socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (p.socket == INVALID_SOCKET || !set_non_blocking(p.socket)) {
        finish(p, false, "socket: " + net::wsa_error_text(::WSAGetLastError()));
        return;
    }
    // Probes always fail fast: a killed backend must show up as refused, not as a probe
    // timeout. A SYN lost on the network is one failed probe, which the hysteresis absorbs.
    net::disable_syn_retransmissions(p.socket);
    p.phase = Phase::Connecting;
    const sockaddr_in& addr = p.backend->address;
    if (::connect(p.socket, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSAEWOULDBLOCK) finish(p, false, "connect: " + net::wsa_error_text(err));
    }
    // Completion (or refusal) shows up in WSAPoll as writable or error.
}

void HealthChecker::advance(Probe& p, short revents) {
    if (p.phase == Phase::Connecting) {
        int err = 0;
        int len = sizeof(err);
        ::getsockopt(p.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) {
            finish(p, false, "connect: " + net::wsa_error_text(err));
            return;
        }
        if ((revents & POLLWRNORM) == 0) {
            finish(p, false, "connect: connection failed");
            return;
        }
        if (p.config.type == HealthConfig::Type::Tcp) {
            finish(p, true, {});
            return;
        }
        p.phase = Phase::Sending;
    }

    if (p.phase == Phase::Sending) {
        const int n = ::send(p.socket, p.request.data() + p.sent, static_cast<int>(p.request.size() - p.sent), 0);
        if (n == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err != WSAEWOULDBLOCK) finish(p, false, "send: " + net::wsa_error_text(err));
            return;
        }
        p.sent += static_cast<std::size_t>(n);
        if (p.sent == p.request.size()) p.phase = Phase::Receiving;
        return;
    }

    if (p.phase == Phase::Receiving) {
        char buf[512];
        const int n = ::recv(p.socket, buf, static_cast<int>(sizeof(buf)), 0);
        if (n == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err != WSAEWOULDBLOCK) finish(p, false, "recv: " + net::wsa_error_text(err));
            return;
        }
        if (n == 0) {
            finish(p, false, "closed before a status line");
            return;
        }
        p.received.append(buf, static_cast<std::size_t>(n));
        const auto eol = p.received.find("\r\n");
        if (eol == std::string::npos) {
            if (p.received.size() > kMaxStatusLineBytes) finish(p, false, "status line too long");
            return;
        }
        const int status = parse_status(std::string_view(p.received).substr(0, eol));
        if (status == 0) finish(p, false, "malformed status line");
        else if (status >= 200 && status < 400) finish(p, true, {});
        else finish(p, false, "HTTP " + std::to_string(status));
    }
}

void HealthChecker::finish(Probe& p, bool ok, std::string detail) {
    if (p.socket != INVALID_SOCKET) ::closesocket(p.socket);
    p.socket = INVALID_SOCKET;
    p.phase = Phase::Idle;

    backend::BackendRuntime& b = *p.backend;
    b.probes.fetch_add(1, std::memory_order_relaxed);
    const bool currently_up = b.state.load(std::memory_order_acquire) != BackendState::Unhealthy;
    const std::uint64_t mark_downs = b.times_marked_down.load(std::memory_order_acquire);
    if (mark_downs != p.seen_mark_downs) {  // marked down since the last probe (by real traffic)
        p.seen_mark_downs = mark_downs;
        p.hysteresis.restart_successes();
    }
    const Hysteresis::Change change = p.hysteresis.record(ok, currently_up);
    b.probe_failures_in_a_row.store(p.hysteresis.failures_in_a_row(), std::memory_order_relaxed);
    b.probe_successes_in_a_row.store(p.hysteresis.successes_in_a_row(), std::memory_order_relaxed);
    b.set_last_probe_error(ok ? std::string() : detail);

    if (change == Hysteresis::Change::MarkDown && b.mark_unhealthy()) {
        p.seen_mark_downs = b.times_marked_down.load(std::memory_order_acquire);
        if (listener_) listener_({b.id, false, detail, p.hysteresis.failures_in_a_row()});
    } else if (change == Hysteresis::Change::MarkUp && b.mark_healthy()) {
        if (listener_) listener_({b.id, true, "probe succeeded", p.hysteresis.successes_in_a_row()});
    }
}

}  // namespace lb::health
