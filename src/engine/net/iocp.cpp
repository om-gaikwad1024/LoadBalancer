#include "net/iocp.h"

#include "net/winsock.h"

namespace lb::net {

namespace {

constexpr ULONG_PTR kQuitKey = 1;

}  // namespace

bool CompletionPort::create(std::string* error) {
    port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (port_ == nullptr) {
        *error = "CreateIoCompletionPort failed: " + wsa_error_text(static_cast<int>(::GetLastError()));
        return false;
    }
    return true;
}

bool CompletionPort::associate(SOCKET s) noexcept {
    return ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), port_, 0, 0) == port_;
}

void CompletionPort::close() noexcept {
    if (port_ != nullptr) {
        ::CloseHandle(port_);
        port_ = nullptr;
    }
}

void WorkerPool::start(CompletionPort& port, std::uint32_t threads) {
    port_ = port.handle();
    threads_.reserve(threads);
    for (std::uint32_t i = 0; i < threads; ++i) {
        threads_.emplace_back([p = port_] {
            ::SetThreadDescription(::GetCurrentThread(), L"lb-iocp-worker");
            run(p);
        });
    }
}

void WorkerPool::stop() {
    for (std::size_t i = 0; i < threads_.size(); ++i) {
        ::PostQueuedCompletionStatus(port_, 0, kQuitKey, nullptr);
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void WorkerPool::run(HANDLE port) noexcept {
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, INFINITE);
        if (overlapped == nullptr) {
            if (key == kQuitKey || !ok) return;  // quit packet, or the port was closed
            continue;
        }
        const DWORD error = ok ? 0 : ::GetLastError();
        IoOp* op = CONTAINING_RECORD(overlapped, IoOp, overlapped);
        op->handler->on_io_complete(op, bytes, error);
    }
}

}  // namespace lb::net
