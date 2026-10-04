#pragma once

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace lb::net {

class IoHandler;

// One overlapped operation. Its owner keeps it alive until the completion arrives.
struct IoOp {
    OVERLAPPED overlapped{};
    IoHandler* handler = nullptr;
    SOCKET socket = INVALID_SOCKET;

    void prepare(IoHandler* h, SOCKET s) noexcept {
        overlapped = {};
        handler = h;
        socket = s;
    }
};

// Receives completions on an IOCP worker thread. Must not block (plan V, VIII).
class IoHandler {
public:
    // error is 0 on success, otherwise a Win32/Winsock error code.
    virtual void on_io_complete(IoOp* op, DWORD bytes, DWORD error) noexcept = 0;

protected:
    ~IoHandler() = default;
};

class CompletionPort {
public:
    CompletionPort() = default;
    ~CompletionPort() { close(); }
    CompletionPort(const CompletionPort&) = delete;
    CompletionPort& operator=(const CompletionPort&) = delete;

    bool create(std::string* error);
    bool associate(SOCKET s) noexcept;
    void close() noexcept;
    HANDLE handle() const noexcept { return port_; }

private:
    HANDLE port_ = nullptr;
};

// Fixed pool of IOCP worker threads created at startup (plan II.2). No thread per connection.
class WorkerPool {
public:
    WorkerPool() = default;
    ~WorkerPool() { stop(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start(CompletionPort& port, std::uint32_t threads);
    // Posts one quit packet per worker and joins them all.
    void stop();
    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(threads_.size()); }

private:
    static void run(HANDLE port) noexcept;

    HANDLE port_ = nullptr;
    std::vector<std::thread> threads_;
};

}  // namespace lb::net
