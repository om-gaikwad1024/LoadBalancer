#include "proxy/client_session.h"

#include <ws2tcpip.h>

#include <cstring>
#include <utility>

namespace lb::proxy {

void ClientSession::InBuffer::consume(std::size_t n) noexcept {
    begin_ += n;
    if (begin_ >= end_) begin_ = end_ = 0;
}

char* ClientSession::InBuffer::prepare(std::size_t n) {
    if (begin_ > 0) {
        std::memmove(data_.data(), data_.data() + begin_, end_ - begin_);
        end_ -= begin_;
        begin_ = 0;
    }
    if (data_.size() < end_ + n) data_.resize(end_ + n);
    return data_.data() + end_;
}

ClientSession::ClientSession(SessionContext& ctx, SOCKET client, const sockaddr_in& peer, bool reject)
    : ctx_(ctx),
      client_(client),
      peer_(peer),
      request_parser_(http::HttpParser::Kind::Request, http::request_parser_limits(ctx.config->current()->limits)),
      response_parser_(http::HttpParser::Kind::Response, http::response_parser_limits(ctx.config->current()->limits)) {
    const auto config = ctx_.config->current();
    client_read_size_ = config->buffers.client_read_bytes;
    backend_read_size_ = config->buffers.backend_read_bytes;
    ctx_.counters->connections_active.fetch_add(1, std::memory_order_relaxed);

    if (reject) {
        ctx_.counters->connections_rejected.fetch_add(1, std::memory_order_relaxed);
        client_out_ = error_response(503);
        stage_ = Stage::Response;
        response_done_ = true;
        response_started_ = true;
        response_status_ = 503;
    }
}

ClientSession::~ClientSession() {
    close_all(true);
    ctx_.counters->connections_active.fetch_sub(1, std::memory_order_relaxed);
    ctx_.registry->remove(this);
}

void ClientSession::start() {
    std::lock_guard lock(mutex_);
    advance();
}

void ClientSession::request_shutdown() {
    std::lock_guard lock(mutex_);
    shutdown_requested_ = true;
    if (closed_) return;
    // Idle keep-alive connection waiting for its next request: close it now.
    if (stage_ == Stage::Request && pending_ == Pending::ClientRecv && request_parser_.idle() && !client_in_.has_data()) {
        close_all(false);
    }
}

void ClientSession::force_close() {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    trace(TraceStep::Aborted);
    close_all(true);
}

void ClientSession::on_io_complete(net::IoOp* /*op*/, DWORD bytes, DWORD error) noexcept {
    std::shared_ptr<ClientSession> keep;  // destroyed after the lock is released
    std::lock_guard lock(mutex_);
    keep = std::move(pending_self_);
    const Pending kind = std::exchange(pending_, Pending::None);
    if (closed_) return;
    try {
        complete(kind, bytes, error);
        advance();
    } catch (...) {
        close_all(true);  // allocation failure: drop this connection, never the process
    }
}

// The single driver: decides the next I/O from the current state.
void ClientSession::advance() {
    for (;;) {
        if (closed_) return;

        if (stage_ == Stage::Request) {
            if (!client_out_.empty()) {  // e.g. "100 Continue"
                if (issue_send(Pending::ClientSend, client_, client_out_, client_out_sent_)) return;
                continue;
            }
            if (!backend_out_.empty()) {
                if (!backend_connected_) {
                    if (start_backend_connect()) return;
                    continue;
                }
                if (issue_send(Pending::BackendSend, backend_, backend_out_, backend_out_sent_)) return;
                continue;
            }
            if (request_done_) {
                trace(TraceStep::RequestForwarded);
                stage_ = Stage::Response;
                response_parser_.reset_for_response(is_head_);
                continue;
            }
            if (client_in_.has_data() || client_poll_) {
                client_poll_ = false;
                process_client_input();
                continue;
            }
            if (client_eof_) {
                close_all(false);  // client went away between or during requests
                return;
            }
            if (issue_recv(Pending::ClientRecv, client_, client_in_, client_read_size_)) return;
            continue;
        }

        if (!client_out_.empty()) {
            if (issue_send(Pending::ClientSend, client_, client_out_, client_out_sent_)) return;
            continue;
        }
        if (response_done_) {
            finish_exchange();
            continue;
        }
        if (backend_in_.has_data() || backend_poll_) {
            backend_poll_ = false;
            process_backend_input();
            continue;
        }
        if (backend_eof_) {
            handle_backend_eof();
            continue;
        }
        if (issue_recv(Pending::BackendRecv, backend_, backend_in_, backend_read_size_)) return;
    }
}

void ClientSession::complete(Pending kind, DWORD bytes, DWORD error) {
    switch (kind) {
        case Pending::None:
            break;
        case Pending::ClientRecv:
            if (error != 0 || bytes == 0) client_eof_ = true;
            else client_in_.commit(bytes);
            break;
        case Pending::ClientSend:
            if (error != 0) {
                if (stage_ == Stage::Response && response_started_) trace(TraceStep::Aborted);
                close_all(true);
                return;
            }
            client_out_sent_ += bytes;
            if (client_out_sent_ >= client_out_.size()) {
                client_out_.clear();
                client_out_sent_ = 0;
            }
            if (stage_ == Stage::Response && response_started_) response_flushed_ = true;
            break;
        case Pending::BackendConnect:
            if (error != 0) {
                fail_request(502);
                return;
            }
            ::setsockopt(backend_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
            net::set_no_delay(backend_);
            backend_connected_ = true;
            trace(TraceStep::BackendConnected);
            break;
        case Pending::BackendSend:
            if (error != 0) {
                fail_request(502);
                return;
            }
            backend_out_sent_ += bytes;
            if (backend_out_sent_ >= backend_out_.size()) {
                backend_out_.clear();
                backend_out_sent_ = 0;
            }
            break;
        case Pending::BackendRecv:
            if (error != 0 || bytes == 0) backend_eof_ = true;
            else backend_in_.commit(bytes);
            break;
    }
}

bool ClientSession::begin_io(Pending kind, SOCKET s) {
    op_.prepare(this, s);
    pending_ = kind;
    pending_self_ = shared_from_this();
    return true;
}

void ClientSession::io_failed_immediately(Pending kind, int error) {
    pending_ = Pending::None;
    pending_self_.reset();  // the caller still holds a reference
    complete(kind, 0, static_cast<DWORD>(error));
}

bool ClientSession::issue_recv(Pending kind, SOCKET s, InBuffer& buffer, std::size_t read_size) {
    WSABUF wsabuf{static_cast<ULONG>(read_size), buffer.prepare(read_size)};
    DWORD flags = 0;
    begin_io(kind, s);
    if (::WSARecv(s, &wsabuf, 1, nullptr, &flags, &op_.overlapped, nullptr) == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            io_failed_immediately(kind, err);
            return false;
        }
    }
    return true;  // completion (even an immediate success) arrives through the port
}

bool ClientSession::issue_send(Pending kind, SOCKET s, const std::string& out, std::size_t sent) {
    WSABUF wsabuf{static_cast<ULONG>(out.size() - sent), const_cast<char*>(out.data() + sent)};
    begin_io(kind, s);
    if (::WSASend(s, &wsabuf, 1, nullptr, 0, &op_.overlapped, nullptr) == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            io_failed_immediately(kind, err);
            return false;
        }
    }
    return true;
}

bool ClientSession::start_backend_connect() {
    backend_ = net::make_overlapped_tcp_socket();
    if (backend_ == INVALID_SOCKET) {
        complete(Pending::BackendConnect, 0, static_cast<DWORD>(::WSAGetLastError()));
        return false;
    }
    sockaddr_in any{};
    any.sin_family = AF_INET;
    if (::bind(backend_, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == SOCKET_ERROR) {
        complete(Pending::BackendConnect, 0, static_cast<DWORD>(::WSAGetLastError()));
        return false;
    }
    if (!ctx_.port->associate(backend_)) {
        complete(Pending::BackendConnect, 0, ::GetLastError());
        return false;
    }
    ctx_.counters->backend_connections_opened.fetch_add(1, std::memory_order_relaxed);
    begin_io(Pending::BackendConnect, backend_);
    if (!ctx_.ext->connect_ex(backend_, reinterpret_cast<const sockaddr*>(&backend_addr_), sizeof(backend_addr_),
                              nullptr, 0, nullptr, &op_.overlapped)) {
        const int err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            io_failed_immediately(Pending::BackendConnect, err);
            return false;
        }
    }
    return true;
}

void ClientSession::process_client_input() {
    const auto r = request_parser_.parse(client_in_.view());
    client_in_.consume(r.consumed);
    switch (r.event) {
        case http::ParseEvent::NeedMore:
            break;
        case http::ParseEvent::HeadComplete:
            client_poll_ = true;
            on_request_head();
            break;
        case http::ParseEvent::Body:
            client_poll_ = true;
            if (backend_chunked_) append_chunk(backend_out_, r.body);
            else backend_out_.append(r.body);
            break;
        case http::ParseEvent::MessageComplete:
            if (backend_chunked_) append_last_chunk(backend_out_);
            request_done_ = true;
            break;
        case http::ParseEvent::Error:
            fail_request(request_parser_.error().status);
            break;
    }
}

void ClientSession::on_request_head() {
    const http::RequestHead& req = request_parser_.request();
    request_id_ = ctx_.next_request_id->fetch_add(1, std::memory_order_relaxed) + 1;
    trace(TraceStep::RequestReceived);

    config_ = ctx_.config->current();
    client_version_ = req.version;
    request_keep_alive_ = req.keep_alive;
    is_head_ = req.method == "HEAD";
    backend_chunked_ = req.framing == http::BodyFraming::Chunked;

    // Step 1.4: every request goes to the first backend of the default group.
    // The registry (1.5) and load balancer (1.7) replace this selection.
    const GroupConfig* group = config_->find_group(config_->routing.default_group);
    if (group == nullptr || group->backends.empty()) {
        fail_request(503);
        return;
    }
    const BackendConfig& backend = group->backends.front();
    backend_addr_ = {};
    backend_addr_.sin_family = AF_INET;
    backend_addr_.sin_port = ::htons(backend.port);
    ::inet_pton(AF_INET, backend.address.c_str(), &backend_addr_.sin_addr);
    backend_host_ = backend.address + ":" + std::to_string(backend.port);
    trace(TraceStep::BackendSelected);

    // The proxy answers Expect: 100-continue itself and strips it, so the client sends
    // its body without waiting on the backend (RFC 9110 10.1.1).
    if (req.version == http::Version::Http11 && req.framing != http::BodyFraming::None &&
        req.fields.has_token("Expect", "100-continue")) {
        client_out_ = "HTTP/1.1 100 Continue\r\n\r\n";
    }
    append_backend_request_head(backend_out_, req, backend_host_);
}

void ClientSession::process_backend_input() {
    const auto r = response_parser_.parse(backend_in_.view());
    backend_in_.consume(r.consumed);
    switch (r.event) {
        case http::ParseEvent::NeedMore:
            break;
        case http::ParseEvent::HeadComplete:
            backend_poll_ = true;
            on_response_head();
            break;
        case http::ParseEvent::Body:
            backend_poll_ = true;
            if (client_mode_ == ClientBodyMode::Chunked) append_chunk(client_out_, r.body);
            else client_out_.append(r.body);
            break;
        case http::ParseEvent::MessageComplete:
            if (interim_) {  // 1xx interim responses are not forwarded
                interim_ = false;
                response_parser_.reset_for_response(is_head_);
                break;
            }
            if (client_mode_ == ClientBodyMode::Chunked) append_last_chunk(client_out_);
            response_done_ = true;
            break;
        case http::ParseEvent::Error:
            backend_response_failed();
            break;
    }
}

void ClientSession::on_response_head() {
    const http::ResponseHead& resp = response_parser_.response();
    if (resp.status < 200) {
        interim_ = true;
        return;
    }
    response_status_ = resp.status;
    trace(TraceStep::ResponseReceived, resp.status);
    client_mode_ = choose_client_body_mode(resp, client_version_);
    keep_alive_ = request_keep_alive_ && client_mode_ != ClientBodyMode::UntilClose && !shutdown_requested_ &&
                  !ctx_.stopping->load(std::memory_order_relaxed);
    append_client_response_head(client_out_, resp, client_mode_, keep_alive_, client_version_);
    response_started_ = true;
}

void ClientSession::handle_backend_eof() {
    backend_eof_ = false;
    const auto r = response_parser_.finish();
    if (r.event == http::ParseEvent::MessageComplete && response_started_ && !interim_) {
        if (client_mode_ == ClientBodyMode::Chunked) append_last_chunk(client_out_);
        response_done_ = true;  // close-delimited body ended
        return;
    }
    backend_response_failed();
}

// The backend broke its response. If nothing reached the client yet, answer 502;
// otherwise close the client connection so it sees an incomplete response rather
// than a corrupted one (plan VI).
void ClientSession::backend_response_failed() {
    if (!response_flushed_) {
        fail_request(502);
        return;
    }
    trace(TraceStep::Aborted);
    close_all(true);
}

void ClientSession::fail_request(int status) {
    close_backend();
    trace(TraceStep::ErrorResponse, status);
    ctx_.counters->error_responses.fetch_add(1, std::memory_order_relaxed);
    client_out_ = error_response(status);
    client_out_sent_ = 0;
    backend_out_.clear();
    backend_out_sent_ = 0;
    backend_in_.clear();
    backend_eof_ = false;
    backend_poll_ = false;
    interim_ = false;
    keep_alive_ = false;
    response_started_ = true;
    response_done_ = true;
    response_status_ = status;
    stage_ = Stage::Response;
}

void ClientSession::finish_exchange() {
    close_backend();  // step 1.4: no pooling yet
    if (request_id_ != 0) {
        trace(TraceStep::ResponseCompleted, response_status_);
        ctx_.counters->requests_completed.fetch_add(1, std::memory_order_relaxed);
    }
    if (!keep_alive_ || shutdown_requested_ || ctx_.stopping->load(std::memory_order_relaxed)) {
        close_all(false);
        return;
    }
    reset_for_next_request();
}

void ClientSession::reset_for_next_request() {
    request_parser_.reset();
    response_parser_.reset();
    backend_out_.clear();
    backend_out_sent_ = 0;
    backend_in_.clear();
    config_.reset();
    request_id_ = 0;
    stage_ = Stage::Request;
    client_poll_ = false;
    backend_poll_ = false;
    backend_eof_ = false;
    request_done_ = false;
    response_done_ = false;
    request_keep_alive_ = false;
    keep_alive_ = false;
    is_head_ = false;
    backend_chunked_ = false;
    interim_ = false;
    response_started_ = false;
    response_flushed_ = false;
    response_status_ = 0;
    client_mode_ = ClientBodyMode::None;
}

void ClientSession::close_backend() noexcept {
    if (backend_ != INVALID_SOCKET) {
        ::closesocket(backend_);
        backend_ = INVALID_SOCKET;
    }
    backend_connected_ = false;
}

void ClientSession::close_all(bool abortive) noexcept {
    closed_ = true;
    close_backend();
    if (client_ != INVALID_SOCKET) {
        if (abortive) {
            linger lg{1, 0};  // RST: the client must not mistake a cut-off response for a complete one
            ::setsockopt(client_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        } else {
            ::shutdown(client_, SD_SEND);
        }
        ::closesocket(client_);  // an outstanding I/O completes with an error and releases the session
        client_ = INVALID_SOCKET;
    }
}

void ClientSession::trace(TraceStep step, int status) const noexcept {
    if (TraceSink* sink = ctx_.trace->load(std::memory_order_acquire)) {
        sink->on_trace(TraceEvent{request_id_, step, status});
    }
}

}  // namespace lb::proxy
