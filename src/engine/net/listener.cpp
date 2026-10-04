#include "net/listener.h"

#include <ws2tcpip.h>

namespace lb::net {

bool Listener::start(const ListenConfig& config, std::string* error) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(config.port);
    if (::inet_pton(AF_INET, config.address.c_str(), &addr.sin_addr) != 1) {
        *error = "invalid listen address: " + config.address;
        return false;
    }

    listen_socket_ = make_overlapped_tcp_socket();
    if (listen_socket_ == INVALID_SOCKET) {
        *error = "socket() failed: " + wsa_error_text(::WSAGetLastError());
        return false;
    }
    BOOL exclusive = TRUE;
    ::setsockopt(listen_socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                 sizeof(exclusive));
    if (::bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        *error = "bind " + config.address + ":" + std::to_string(config.port) + " failed: " +
                 wsa_error_text(::WSAGetLastError());
        return false;
    }
    if (::listen(listen_socket_, static_cast<int>(config.backlog)) == SOCKET_ERROR) {
        *error = "listen failed: " + wsa_error_text(::WSAGetLastError());
        return false;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    ::getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&bound), &len);
    bound_port_ = ::ntohs(bound.sin_port);

    if (!port_.associate(listen_socket_)) {
        *error = "associating the listener with the IOCP failed: " + wsa_error_text(static_cast<int>(::GetLastError()));
        return false;
    }

    for (std::uint32_t i = 0; i < config.pending_accepts; ++i) {
        ops_.push_back(std::make_unique<AcceptOp>());
        if (!post_accept(ops_.back().get())) {
            *error = "AcceptEx failed: " + wsa_error_text(::WSAGetLastError());
            return false;
        }
    }
    return true;
}

// The listening socket is only used and closed under mutex_, so no AcceptEx can ever run
// on a closed (and possibly reused) handle value.
void Listener::stop() {
    std::unique_lock lock(mutex_);
    if (stopping_.exchange(true)) return;
    if (listen_socket_ != INVALID_SOCKET) {
        ::closesocket(listen_socket_);  // completes every outstanding AcceptEx with an error
        listen_socket_ = INVALID_SOCKET;
    }
    drained_.wait(lock, [this] { return outstanding_ == 0; });
}

bool Listener::post_accept(AcceptOp* op) noexcept {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    op->accept_socket = make_overlapped_tcp_socket();
    if (op->accept_socket == INVALID_SOCKET) return false;
    op->prepare(this, listen_socket_);
    ++outstanding_;
    DWORD received = 0;
    if (!ext_.accept_ex(listen_socket_, op->accept_socket, op->addresses, 0, kAddressLength, kAddressLength, &received,
                        &op->overlapped) &&
        ::WSAGetLastError() != ERROR_IO_PENDING) {
        ::closesocket(op->accept_socket);
        op->accept_socket = INVALID_SOCKET;
        if (--outstanding_ == 0) drained_.notify_all();
        return false;
    }
    return true;
}

void Listener::finished_one() noexcept {
    std::lock_guard lock(mutex_);
    if (--outstanding_ == 0) drained_.notify_all();
}

void Listener::on_io_complete(IoOp* io, DWORD /*bytes*/, DWORD error) noexcept {
    auto* op = static_cast<AcceptOp*>(io);
    SOCKET accepted = op->accept_socket;
    op->accept_socket = INVALID_SOCKET;

    bool deliver = false;
    if (error == 0) {
        std::lock_guard lock(mutex_);
        if (!stopping_) {
            SOCKET listener = listen_socket_;
            deliver = ::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                   reinterpret_cast<const char*>(&listener), sizeof(listener)) == 0;
        }
    }
    if (deliver) {
        sockaddr* local = nullptr;
        sockaddr* remote = nullptr;
        int local_len = 0;
        int remote_len = 0;
        ext_.get_accept_ex_sockaddrs(op->addresses, 0, kAddressLength, kAddressLength, &local, &local_len, &remote,
                                     &remote_len);
        sockaddr_in peer{};
        if (remote != nullptr && remote_len >= static_cast<int>(sizeof(sockaddr_in))) {
            peer = *reinterpret_cast<const sockaddr_in*>(remote);
        }
        sink_.on_accepted(accepted, peer);
    } else if (accepted != INVALID_SOCKET) {
        ::closesocket(accepted);
    }

    // Keep the same number of accepts outstanding until stop().
    post_accept(op);
    finished_one();
}

}  // namespace lb::net
