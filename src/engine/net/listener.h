#pragma once

#include <winsock2.h>
#include <ws2ipdef.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.h"
#include "net/iocp.h"
#include "net/winsock.h"

namespace lb::net {

class AcceptSink {
public:
    // Takes ownership of an accepted socket (already associated with nothing).
    virtual void on_accepted(SOCKET s, const sockaddr_in& peer) noexcept = 0;

protected:
    ~AcceptSink() = default;
};

// Accepts connections with AcceptEx on the IOCP (plan IV.1), keeping a fixed number of
// accepts outstanding.
class Listener final : public IoHandler {
public:
    Listener(CompletionPort& port, const SocketExtensions& ext, AcceptSink& sink) noexcept
        : port_(port), ext_(ext), sink_(sink) {}
    ~Listener() { stop(); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    bool start(const ListenConfig& config, std::string* error);
    // Stops accepting: closes the listening socket and waits until every outstanding
    // AcceptEx has completed and its socket is closed.
    void stop();

    std::uint16_t port() const noexcept { return bound_port_; }

    void on_io_complete(IoOp* op, DWORD bytes, DWORD error) noexcept override;

private:
    // AcceptEx needs room for both addresses, each sizeof(address) + 16 bytes.
    static constexpr DWORD kAddressLength = sizeof(sockaddr_in6) + 16;

    struct AcceptOp : IoOp {
        SOCKET accept_socket = INVALID_SOCKET;
        char addresses[2 * kAddressLength] = {};
    };

    bool post_accept(AcceptOp* op) noexcept;
    void finished_one() noexcept;

    CompletionPort& port_;
    const SocketExtensions& ext_;
    AcceptSink& sink_;
    SOCKET listen_socket_ = INVALID_SOCKET;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stopping_{false};
    std::vector<std::unique_ptr<AcceptOp>> ops_;

    std::mutex mutex_;
    std::condition_variable drained_;
    std::size_t outstanding_ = 0;
};

}  // namespace lb::net
