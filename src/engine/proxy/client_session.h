#pragma once

#include <winsock2.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "http/http_parser.h"
#include "net/iocp.h"
#include "proxy/http_writer.h"
#include "proxy/session_context.h"

namespace lb::proxy {

// One client connection and the requests on it, as a state machine advanced only by
// IOCP completions (plan IV.18). At most one I/O is outstanding at a time, so a slow
// reader on either side applies back-pressure instead of growing buffers, and no
// worker ever blocks. The per-session mutex only orders a completion against
// shutdown calls from the engine thread; there is no shared lock across sessions.
class ClientSession final : public net::IoHandler, public std::enable_shared_from_this<ClientSession> {
public:
    // `reject` = over the global connection limit: answer 503 and close (plan IV.1).
    ClientSession(SessionContext& ctx, SOCKET client, const sockaddr_in& peer, bool reject);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void start();
    // Engine is stopping: close now if idle between requests, else after the current response.
    void request_shutdown();
    // Shutdown grace expired: close both sockets immediately.
    void force_close();

    void on_io_complete(net::IoOp* op, DWORD bytes, DWORD error) noexcept override;

private:
    enum class Stage : std::uint8_t { Request, Response };
    enum class Pending : std::uint8_t { None, ClientRecv, ClientSend, BackendConnect, BackendSend, BackendRecv };

    // Receive buffer: bytes not yet consumed by the parser.
    class InBuffer {
    public:
        std::string_view view() const noexcept { return {data_.data() + begin_, end_ - begin_}; }
        bool has_data() const noexcept { return end_ > begin_; }
        void consume(std::size_t n) noexcept;
        char* prepare(std::size_t n);
        void commit(std::size_t n) noexcept { end_ += n; }
        void clear() noexcept { begin_ = end_ = 0; }

    private:
        std::vector<char> data_;
        std::size_t begin_ = 0;
        std::size_t end_ = 0;
    };

    void advance();
    void complete(Pending kind, DWORD bytes, DWORD error);
    bool issue_recv(Pending kind, SOCKET s, InBuffer& buffer, std::size_t read_size);
    bool issue_send(Pending kind, SOCKET s, const std::string& out, std::size_t sent);
    bool start_backend_connect();
    bool begin_io(Pending kind, SOCKET s);
    void io_failed_immediately(Pending kind, int error);

    void process_client_input();
    void on_request_head();
    void process_backend_input();
    void on_response_head();
    void handle_backend_eof();
    void backend_response_failed();
    void fail_request(int status);
    void finish_exchange();
    void reset_for_next_request();

    void close_backend() noexcept;
    void close_all(bool abortive) noexcept;
    void trace(TraceStep step, int status = 0) const noexcept;

    SessionContext& ctx_;
    std::mutex mutex_;
    net::IoOp op_;
    Pending pending_ = Pending::None;
    std::shared_ptr<ClientSession> pending_self_;  // keeps the session alive while an I/O is outstanding
    bool closed_ = false;
    bool shutdown_requested_ = false;

    SOCKET client_ = INVALID_SOCKET;
    sockaddr_in peer_{};
    SOCKET backend_ = INVALID_SOCKET;
    bool backend_connected_ = false;
    sockaddr_in backend_addr_{};
    std::string backend_host_;

    std::size_t client_read_size_ = 0;
    std::size_t backend_read_size_ = 0;
    http::HttpParser request_parser_;
    http::HttpParser response_parser_;

    InBuffer client_in_;
    InBuffer backend_in_;
    std::string client_out_;
    std::size_t client_out_sent_ = 0;
    std::string backend_out_;
    std::size_t backend_out_sent_ = 0;

    // Per-request state.
    std::shared_ptr<const ConfigSnapshot> config_;  // captured when the request starts (plan II.7)
    std::uint64_t request_id_ = 0;
    Stage stage_ = Stage::Request;
    bool client_poll_ = false;   // parser may report more without new input
    bool backend_poll_ = false;
    bool client_eof_ = false;
    bool backend_eof_ = false;
    bool request_done_ = false;
    bool response_done_ = false;
    bool request_keep_alive_ = false;
    bool keep_alive_ = false;
    bool is_head_ = false;
    bool backend_chunked_ = false;
    bool interim_ = false;
    bool response_started_ = false;   // final response head queued for the client
    bool response_flushed_ = false;   // some final-response bytes already reached the client
    int response_status_ = 0;
    http::Version client_version_ = http::Version::Http11;
    ClientBodyMode client_mode_ = ClientBodyMode::None;
};

}  // namespace lb::proxy
