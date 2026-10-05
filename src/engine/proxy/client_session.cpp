#include "proxy/client_session.h"

#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <utility>

#include "core/debug_assert.h"
#include "routing/router.h"

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
    timeouts_ = config->timeouts;
    peer_address_ = ::ntohl(peer.sin_addr.s_addr);
    peer_ip_ = format_ipv4(peer_address_);
    // The first request's head must arrive within client_header_ms of the accept, so a
    // connection that sends nothing is closed too (slowloris).
    head_deadline_ = Clock::now() + std::chrono::milliseconds(timeouts_.client_header_ms);
    head_deadline_set_ = true;
    ctx_.counters->connections_active.fetch_add(1, std::memory_order_relaxed);

    if (reject) {
        ctx_.counters->connections_rejected.fetch_add(1, std::memory_order_relaxed);
        client_out_ = error_response(503);
        stage_ = Stage::Response;
        response_done_ = true;
        response_started_ = true;
        proxy_error_ = true;
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

bool ClientSession::abort_for_drain(const backend::BackendRuntime* backend) {
    std::lock_guard lock(mutex_);
    if (closed_ || drain_abort_ || !backend_in_use_ || backend_rt_.get() != backend) return false;
    drain_abort_ = true;
    if (pending_ == Pending::PoolWait) {
        // Still queued: wake it now. Already granted: its wake is on the way.
        if (ticket_ && backend_rt_->pool.cancel(ticket_)) ticket_->wake();
    } else if (pending_ != Pending::None && op_.socket != INVALID_SOCKET) {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op_.socket), &op_.overlapped);
    }
    return true;
}

void ClientSession::on_io_complete(net::IoOp* /*op*/, DWORD bytes, DWORD error) noexcept {
    std::shared_ptr<ClientSession> keep;  // destroyed after the lock is released
    std::lock_guard lock(mutex_);
    keep = std::move(pending_self_);
    const Pending kind = std::exchange(pending_, Pending::None);
    if (closed_) {
        if (kind == Pending::PoolWait) on_pool_ticket(/*session_closed=*/true);
        return;
    }
    try {
        if (drain_abort_) {
            // The drain timeout cancelled this I/O; whatever it carried is discarded.
            drain_abort_ = false;
            expired_ = Deadline::None;
            if (kind == Pending::PoolWait) on_pool_ticket(/*session_closed=*/true);  // hand back any grant
            handle_drain_abort();
        } else if (expired_ != Deadline::None) {
            // The timer cancelled this I/O; whatever it carried is discarded.
            handle_timeout(std::exchange(expired_, Deadline::None));
        } else {
            complete(kind, bytes, error);
        }
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
                    if (!holding_slot_) {
                        if (acquire_backend_connection()) return;  // queued for a slot
                        continue;
                    }
                    if (start_backend_connect()) return;
                    continue;
                }
                if (issue_send(Pending::BackendSend, backend_, backend_out_, backend_out_sent_)) return;
                continue;
            }
            if (request_done_) {
                trace(TraceStep::RequestForwarded);
                response_deadline_ = Clock::now() + std::chrono::milliseconds(timeouts_.backend_response_ms);
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
            if (error != 0 || bytes == 0) {
                client_eof_ = true;
            } else {
                client_in_.commit(bytes);
                if (!head_deadline_set_) {  // first byte of a request on a kept-alive connection
                    head_deadline_ = Clock::now() + std::chrono::milliseconds(timeouts_.client_header_ms);
                    head_deadline_set_ = true;
                }
                if (!request_started_) {  // total latency starts at the request's first byte
                    request_start_ = Clock::now();
                    request_started_ = true;
                }
            }
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
        case Pending::PoolWait:
            on_pool_ticket(/*session_closed=*/false);
            break;
        case Pending::BackendConnect:
            if (error != 0) {
                fail_request(502, /*backend_fault=*/true, "connect: " + net::wsa_error_text(static_cast<int>(error)));
                return;
            }
            ::setsockopt(backend_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
            net::set_no_delay(backend_);
            backend_connected_ = true;
            backend_ready_at_ = Clock::now();
            trace(TraceStep::BackendConnected);
            break;
        case Pending::BackendSend:
            if (error != 0) {
                if (try_stale_retry()) break;
                fail_request(502, /*backend_fault=*/true, "send: " + net::wsa_error_text(static_cast<int>(error)));
                return;
            }
            backend_out_sent_ += bytes;
            if (backend_out_sent_ >= backend_out_.size()) {
                backend_out_.clear();
                backend_out_sent_ = 0;
            }
            break;
        case Pending::BackendRecv:
            if (error != 0 || bytes == 0) {
                backend_eof_ = true;
            } else {
                backend_in_.commit(bytes);
                backend_bytes_received_ += bytes;
            }
            break;
    }
}

bool ClientSession::begin_io(Pending kind, SOCKET s) {
    update_deadline(kind);
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

// Returns true if the request is now queued for a pool slot (an I/O-like wait is pending).
bool ClientSession::acquire_backend_connection() {
    // Prepared before acquire(): once queued, another thread may wake us at any moment.
    begin_io(Pending::PoolWait, INVALID_SOCKET);
    auto ticket = std::make_shared<backend::PoolTicket>();
    ticket->wake = [this] { ::PostQueuedCompletionStatus(ctx_.port->handle(), 0, 0, &op_.overlapped); };

    SOCKET s = INVALID_SOCKET;
    const auto result = backend_rt_->pool.acquire(&s, ticket, std::exchange(prefer_new_connection_, false));
    if (result == backend::ConnectionPool::Acquire::Queued) {
        ticket_ = std::move(ticket);
        const auto wait = std::chrono::milliseconds(config_->pool.wait_timeout_ms);
        wait_timer_ = ctx_.timers->schedule(Clock::now() + wait, [rt = backend_rt_, t = ticket_] {
            if (rt->pool.cancel(t)) t->wake();  // still queued: wake it with no connection (503)
        });
        return true;
    }

    pending_ = Pending::None;
    pending_self_.reset();
    switch (result) {
        case backend::ConnectionPool::Acquire::Reused:
            backend_ = s;
            holding_slot_ = true;
            backend_connected_ = true;
            reused_connection_ = true;
            backend_ready_at_ = Clock::now();
            ctx_.counters->backend_connections_reused.fetch_add(1, std::memory_order_relaxed);
            trace(TraceStep::BackendConnected);
            break;
        case backend::ConnectionPool::Acquire::Connect:
            holding_slot_ = true;
            break;
        case backend::ConnectionPool::Acquire::Rejected:
        case backend::ConnectionPool::Acquire::Queued:
            ctx_.counters->pool_rejections.fetch_add(1, std::memory_order_relaxed);
            log_event("pool_rejected",
                      backend_rt_->id + " is at max_connections_per_backend and its wait queue is full: 503",
                      {{"reason", "queue_full"}});
            fail_request(503, /*backend_fault=*/false);
            break;
    }
    return false;
}

// The queued ticket was settled: granted a connection, granted a slot, timed out, or
// cancelled because this session closed.
void ClientSession::on_pool_ticket(bool session_closed) {
    if (wait_timer_ != 0) ctx_.timers->cancel(wait_timer_);
    wait_timer_ = 0;
    auto t = std::move(ticket_);
    if (!t) return;

    if (session_closed) {  // hand back whatever was granted
        if (t->socket != INVALID_SOCKET) backend_rt_->pool.release(t->socket, false, Clock::now());
        else if (t->may_connect) backend_rt_->pool.abandon_slot();
        return;
    }
    if (t->socket != INVALID_SOCKET) {
        backend_ = t->socket;
        holding_slot_ = true;
        backend_connected_ = true;
        reused_connection_ = true;
        backend_ready_at_ = Clock::now();
        ctx_.counters->backend_connections_reused.fetch_add(1, std::memory_order_relaxed);
        trace(TraceStep::BackendConnected);
    } else if (t->may_connect) {
        holding_slot_ = true;
    } else {
        ctx_.counters->pool_rejections.fetch_add(1, std::memory_order_relaxed);
        log_event("pool_rejected",
                  "waited " + std::to_string(config_->pool.wait_timeout_ms) + " ms for a connection to " +
                      backend_rt_->id + ": 503",
                  {{"reason", "wait_timeout"}});
        fail_request(503, /*backend_fault=*/false);
    }
}

bool ClientSession::start_backend_connect() {
    backend_ = net::make_overlapped_tcp_socket();
    if (backend_ == INVALID_SOCKET) {
        complete(Pending::BackendConnect, 0, static_cast<DWORD>(::WSAGetLastError()));
        return false;
    }
    if (config_->pool.fail_fast_connect) net::disable_syn_retransmissions(backend_);
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
    if (!ctx_.ext->connect_ex(backend_, reinterpret_cast<const sockaddr*>(&backend_rt_->address),
                              sizeof(backend_rt_->address), nullptr, 0, nullptr, &op_.overlapped)) {
        const int err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            io_failed_immediately(Pending::BackendConnect, err);
            return false;
        }
    }
    return true;
}

// Plan VI "stale pooled connection": detected on reuse; resend on a fresh connection when
// that is safe, otherwise the caller answers 502.
bool ClientSession::try_stale_retry() {
    if (!reused_connection_ || stale_retried_ || !retry_safe_ || backend_bytes_received_ > 0) return false;
    stale_retried_ = true;
    ctx_.counters->stale_retries.fetch_add(1, std::memory_order_relaxed);
    log_event("retry", "pooled connection to " + backend_rt_->id +
                           " was closed by the backend before any response; resending on a fresh connection",
              {{"reason", "stale_pooled_connection"}});
    close_backend(false);
    backend_out_ = retry_head_;
    backend_out_sent_ = 0;
    backend_in_.clear();
    backend_eof_ = false;
    backend_poll_ = false;
    prefer_new_connection_ = true;
    stage_ = Stage::Request;
    request_done_ = true;  // the whole request is in retry_head_
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
            // Not logged: malformed input is common and attacker-controlled; it is counted
            // in the metrics as a 4xx/5xx and answered (plan IV.3).
            fail_request(request_parser_.error().status, /*backend_fault=*/false);
            break;
    }
}

void ClientSession::on_request_head() {
    const http::RequestHead& req = request_parser_.request();
    request_id_ = ctx_.next_request_id->fetch_add(1, std::memory_order_relaxed) + 1;
    if (!request_started_) {  // a pipelined request parsed from already-buffered bytes
        request_start_ = Clock::now();
        request_started_ = true;
    }

    // One capture per request: the config and the backends it names come from the same
    // reload, and stay fixed for this request even if a reload lands meanwhile (IV.14).
    topology_ = ctx_.backends->topology();
    config_ = topology_->config;
    timeouts_ = config_->timeouts;
    request_head_seen_ = true;
    const bool peer_trusted = config_->is_trusted_proxy(peer_address_);
    request_tag_ = choose_request_id(req.fields, peer_trusted, ctx_.request_ids->make(request_id_));
    trace(TraceStep::RequestReceived);
    client_version_ = req.version;
    request_keep_alive_ = req.keep_alive;
    is_head_ = req.method == "HEAD";
    backend_chunked_ = req.framing == http::BodyFraming::Chunked;

    // Plan III order: content routing picks the group for every request (IV.8), then the
    // group's balancer picks among its healthy, non-draining backends (IV.7).
    const routing::RouteDecision route = routing::route(config_->routing, req);
    const std::string& group_name = *route.group;
    std::string route_detail;  // built only for the debug event log
    if (ctx_.events != nullptr && ctx_.events->trace_requests()) {
        route_detail = group_name + (route.rule != nullptr ? " (rule " + route.rule->id + ")" : " (default)");
    }
    trace(TraceStep::GroupRouted, 0, route_detail);
    if (const auto group = topology_->find_group(group_name)) {
        balance::PickContext pick;
        pick.now = Clock::now();
        pick.response_time_expiry = std::chrono::milliseconds(config_->balancing.response_time_expiry_ms);
        if (group->strategy == Strategy::IpHash) {
            pick.client_hash = balance::hash_key(client_identity(peer_address_, req.fields, *config_));
        }
        group_config_ = config_->find_group(group_name);
        backend_rt_ = pick_backend(*group, group_config_, req, pick);
    }
    if (!backend_rt_) {
        // Plan IV.7/VI: no eligible backend is answered at once with 503, never a hang, and
        // logged loudly: this is the one case where clients correctly see an error.
        ctx_.counters->no_backend_available.fetch_add(1, std::memory_order_relaxed);
        log_event("no_backend_available", "no healthy, non-draining backend in group " + group_name + ": 503",
                  {{"group", group_name}, {"rule", route.rule != nullptr ? route.rule->id : std::string()}});
        fail_request(503, /*backend_fault=*/false);
        return;
    }
    LB_DEBUG_ASSERT(backend_rt_->eligible(), "a request may only go to a healthy, non-draining backend (IV.4)");
    backend_rt_->in_flight.fetch_add(1, std::memory_order_relaxed);
    backend_rt_->requests.fetch_add(1, std::memory_order_relaxed);
    backend_in_use_ = true;
    metrics_series_ = backend_rt_->metrics_series;
    trace(TraceStep::BackendSelected);

    // The proxy answers Expect: 100-continue itself and strips it, so the client sends
    // its body without waiting on the backend (RFC 9110 10.1.1).
    if (req.version == http::Version::Http11 && req.framing != http::BodyFraming::None &&
        req.fields.has_token("Expect", "100-continue")) {
        client_out_ = "HTTP/1.1 100 Continue\r\n\r\n";
    }

    ForwardingContext fwd;
    fwd.client_ip = peer_ip_;
    fwd.peer_trusted = peer_trusted;
    fwd.proto = "http";  // phase 4: "https" on TLS listeners
    fwd.request_id = request_tag_;
    const GroupConfig* group = config_->find_group(backend_rt_->group);
    fwd.host_mode = group != nullptr ? group->host_header : HostHeaderMode::Preserve;
    fwd.backend_endpoint = backend_rt_->endpoint;
    append_backend_request_head(backend_out_, req, fwd);

    const bool idempotent = req.method == "GET" || req.method == "HEAD" || req.method == "OPTIONS";
    const bool bodiless = req.framing == http::BodyFraming::None ||
                          (req.framing == http::BodyFraming::ContentLength && req.content_length == 0);
    retry_safe_ = idempotent && bodiless;
    if (retry_safe_) retry_head_ = backend_out_;
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
            backend_done_at_ = Clock::now();
            response_done_ = true;
            break;
        case http::ParseEvent::Error:
            backend_response_failed(std::string("bad response: ") + std::string(response_parser_.error().detail));
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
    append_client_response_head(client_out_, resp, client_mode_, keep_alive_, client_version_, request_tag_,
                                sticky_set_cookie_);
    if (sticky_ != nullptr && sticky_->mode == StickyConfig::Mode::ApplicationCookie) learn_sticky_cookie(resp);
    response_started_ = true;
}

namespace {

std::string_view trim_spaces(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string_view unquote(std::string_view v) noexcept {
    return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
}

// The value of the first cookie named `name` in the request's Cookie headers, or empty.
std::string_view request_cookie(const http::Fields& fields, std::string_view name) noexcept {
    for (const auto& f : fields.all()) {
        if (!http::iequals(f.name, "Cookie")) continue;
        std::string_view rest = f.value;
        while (!rest.empty()) {
            const auto semi = rest.find(';');
            const std::string_view pair = trim_spaces(rest.substr(0, semi));
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos && trim_spaces(pair.substr(0, eq)) == name) {
                return unquote(trim_spaces(pair.substr(eq + 1)));
            }
        }
    }
    return {};
}

}  // namespace

std::shared_ptr<backend::BackendRuntime> ClientSession::pick_backend(balance::GroupBalancer& group,
                                                                     const GroupConfig* config,
                                                                     const http::RequestHead& request,
                                                                     const balance::PickContext& pick) {
    if (config == nullptr || config->sticky.mode == StickyConfig::Mode::Off || ctx_.sticky == nullptr) {
        return group.pick(pick);
    }
    sticky_ = &config->sticky;
    sticky_group_ = group.name;
    const auto ttl = std::chrono::milliseconds(sticky_->ttl_ms);
    const bool inserted = sticky_->mode == StickyConfig::Mode::InsertedCookie;
    std::string key(request_cookie(request.fields, sticky_->cookie));
    // A proxy cookie that is not one of ours is ignored; the client then gets a new one.
    if (inserted && !affinity::StickyTable::is_session_key(key)) key.clear();

    std::shared_ptr<backend::BackendRuntime> chosen;
    const bool details = ctx_.events != nullptr && ctx_.events->trace_requests();  // debug trace text
    if (!key.empty()) {
        if (const auto mapped = ctx_.sticky->find(sticky_group_, key, pick.now, ttl)) {
            std::shared_ptr<backend::BackendRuntime> current;
            for (const auto& m : group.members) {
                if (m->id == *mapped) current = m;
            }
            if (current && current->eligible()) {
                ctx_.counters->sticky_hits.fetch_add(1, std::memory_order_relaxed);
                trace(TraceStep::AffinityChecked, 0, details ? "sticky to " + current->id : std::string());
                return current;
            }
            // Plan IV.9: unhealthy, draining (or removed): a new backend in the same group,
            // the session moves there, and the reassignment is logged.
            chosen = group.pick(pick);
            if (!chosen) return nullptr;
            ctx_.sticky->assign(sticky_group_, key, chosen->id, pick.now, ttl);
            ctx_.counters->sticky_reassignments.fetch_add(1, std::memory_order_relaxed);
            const std::string reason =
                current ? std::string(to_string(current->state.load(std::memory_order_acquire))) : "removed from the group";
            log_event("sticky_reassigned",
                      "session in group " + sticky_group_ + " moved from " + *mapped + " (" + reason + ") to " +
                          chosen->id,
                      {{"group", sticky_group_}, {"from", *mapped}, {"to", chosen->id}, {"reason", reason}});
            trace(TraceStep::AffinityChecked, 0,
                  details ? "reassigned from " + *mapped + " (" + reason + ") to " + chosen->id : std::string());
            return chosen;
        }
        // A cookie the table does not know (new, expired, or from before a restart).
        chosen = group.pick(pick);
        if (chosen && ctx_.sticky->assign(sticky_group_, key, chosen->id, pick.now, ttl)) {
            ctx_.counters->sticky_assignments.fetch_add(1, std::memory_order_relaxed);
        }
        trace(TraceStep::AffinityChecked, 0,
              details ? (chosen ? "new session on " + chosen->id : std::string("no backend")) : std::string());
        return chosen;
    }

    // No session cookie yet.
    chosen = group.pick(pick);
    if (chosen && inserted) {
        key = affinity::StickyTable::new_session_key();
        if (ctx_.sticky->assign(sticky_group_, key, chosen->id, pick.now, ttl)) {
            ctx_.counters->sticky_assignments.fetch_add(1, std::memory_order_relaxed);
            sticky_set_cookie_ = sticky_->cookie + "=" + key + "; Path=/; HttpOnly; SameSite=Lax";
        }
    }
    trace(TraceStep::AffinityChecked, 0,
          details ? (chosen ? "no session cookie; " + chosen->id : std::string("no backend")) : std::string());
    return chosen;
}

void ClientSession::learn_sticky_cookie(const http::ResponseHead& response) {
    for (const auto& f : response.fields.all()) {
        if (!http::iequals(f.name, "Set-Cookie")) continue;
        const std::string_view pair = trim_spaces(std::string_view(f.value).substr(0, f.value.find(';')));
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos || trim_spaces(pair.substr(0, eq)) != sticky_->cookie) continue;
        const std::string_view value = unquote(trim_spaces(pair.substr(eq + 1)));
        if (value.empty()) continue;  // a cookie being deleted; its mapping expires on its own
        if (ctx_.sticky->assign(sticky_group_, value, backend_rt_->id, Clock::now(),
                                std::chrono::milliseconds(sticky_->ttl_ms))) {
            ctx_.counters->sticky_assignments.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void ClientSession::handle_backend_eof() {
    backend_eof_ = false;
    // A reused connection that died before any response byte: the backend closed it while idle.
    if (!response_started_ && try_stale_retry()) return;
    const auto r = response_parser_.finish();
    if (r.event == http::ParseEvent::MessageComplete && response_started_ && !interim_) {
        if (client_mode_ == ClientBodyMode::Chunked) append_last_chunk(client_out_);
        backend_done_at_ = Clock::now();
        response_done_ = true;  // close-delimited body ended
        return;
    }
    backend_response_failed("connection closed before the response was complete");
}

// The backend broke its response. If nothing reached the client yet, answer 502;
// otherwise close the client connection so it sees an incomplete response rather
// than a corrupted one (plan VI).
void ClientSession::backend_response_failed(std::string_view reason) {
    if (!response_flushed_) {
        fail_request(502, /*backend_fault=*/true, reason);
        return;
    }
    trace(TraceStep::Aborted);
    log_event("response_aborted",
              backend_rt_->id + " failed mid-response (" + std::string(reason) +
                  "); client connection cut so it sees an incomplete response",
              {{"reason", reason}});
    end_backend_use(BackendOutcome::Failure, reason);
    record_outcome(0);
    close_all(true);
}

void ClientSession::fail_request(int status, bool backend_fault, std::string_view reason) {
    if (backend_fault && backend_rt_) {
        log_event("backend_error",
                  backend_rt_->id + " failed: " + std::string(reason) + " -> " + std::to_string(status),
                  {{"status", status}, {"reason", reason}});
    } else {
        metrics_series_ = 0;  // the proxy refused the request; no backend is charged
    }
    close_backend(false);
    end_backend_use(backend_fault ? BackendOutcome::Failure : BackendOutcome::NotJudged, reason);
    trace(TraceStep::ErrorResponse, status);
    ctx_.counters->error_responses.fetch_add(1, std::memory_order_relaxed);
    client_out_ = error_response(status, request_tag_);
    client_out_sent_ = 0;
    backend_out_.clear();
    backend_out_sent_ = 0;
    backend_in_.clear();
    backend_eof_ = false;
    backend_poll_ = false;
    interim_ = false;
    keep_alive_ = false;
    proxy_error_ = true;
    response_started_ = true;
    response_done_ = true;
    response_status_ = status;
    stage_ = Stage::Response;
}

void ClientSession::finish_exchange() {
    if (!proxy_error_) {
        // Back to the pool only after a cleanly framed, complete response (plan IV.5), and
        // only while the backend may still take traffic.
        const bool reusable = response_parser_.response().keep_alive && backend_rt_ && backend_rt_->eligible() &&
                              !ctx_.stopping->load(std::memory_order_relaxed);
        close_backend(reusable);
        end_backend_use(BackendOutcome::Success);
    } else {
        close_backend(false);
    }
    if (request_id_ != 0) {
        trace(TraceStep::ResponseCompleted, response_status_);
        ctx_.counters->requests_completed.fetch_add(1, std::memory_order_relaxed);
        record_outcome(response_status_);
        if (stale_retried_ && !proxy_error_) {
            ctx_.counters->stale_retry_successes.fetch_add(1, std::memory_order_relaxed);
            log_event("retry_result", "resent request succeeded with " + std::to_string(response_status_),
                      {{"status", response_status_}});
        }
    }
    if (!keep_alive_ || shutdown_requested_ || ctx_.stopping->load(std::memory_order_relaxed)) {
        close_all(false);
        return;
    }
    reset_for_next_request();
}

void ClientSession::end_backend_use(BackendOutcome outcome, std::string_view reason) noexcept {
    if (!backend_in_use_ || !backend_rt_) return;
    backend_in_use_ = false;
    backend_rt_->in_flight.fetch_sub(1, std::memory_order_relaxed);
    if (outcome == BackendOutcome::Success) backend_rt_->successes.fetch_add(1, std::memory_order_relaxed);
    if (outcome == BackendOutcome::Failure) backend_rt_->failures.fetch_add(1, std::memory_order_relaxed);

    // Least response time (plan IV.7): a response is sampled from connection ready to its
    // last byte; a failure counts as taking the whole backend_response_ms, so a backend
    // that fails fast never looks fast. Requests the proxy refused are not samples.
    if (config_ && outcome != BackendOutcome::NotJudged) {
        const TimePoint now = Clock::now();
        const auto decay = std::chrono::milliseconds(config_->balancing.response_time_decay_ms);
        if (outcome == BackendOutcome::Success && backend_ready_at_ != TimePoint{} &&
            backend_done_at_ >= backend_ready_at_) {
            backend_rt_->record_response_time(backend_done_at_ - backend_ready_at_, now, decay);
        } else if (outcome == BackendOutcome::Failure) {
            backend_rt_->record_response_time(std::chrono::milliseconds(config_->timeouts.backend_response_ms), now,
                                              decay);
        }
    }

    if (outcome == BackendOutcome::Failure) {
        record_passive_health(true, reason);
    } else if (outcome == BackendOutcome::Success) {
        const bool count_5xx = group_config_ != nullptr && group_config_->passive_health.count_5xx;
        if (count_5xx && response_status_ >= 500) {
            record_passive_health(true, "HTTP " + std::to_string(response_status_));
        } else {
            record_passive_health(false, {});
        }
    }
}

void ClientSession::record_passive_health(bool failed, std::string_view reason) noexcept {
    if (group_config_ == nullptr || !group_config_->passive_health.enabled) return;
    backend::BackendRuntime& b = *backend_rt_;
    if (!failed) {
        b.passive_failures_in_a_row.store(0, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t threshold = group_config_->passive_health.consecutive_failures;
    const std::uint32_t in_a_row = b.passive_failures_in_a_row.fetch_add(1, std::memory_order_relaxed) + 1;
    // mark_unhealthy() changes only a healthy backend, once: a draining backend stays draining,
    // and concurrent failures cannot mark it down twice.
    if (in_a_row < threshold || !b.mark_unhealthy()) return;
    b.passive_failures_in_a_row.store(0, std::memory_order_relaxed);
    ctx_.counters->backends_marked_down.fetch_add(1, std::memory_order_relaxed);
    log_event("backend_marked_unhealthy",
              b.id + " marked unhealthy after " + std::to_string(in_a_row) + " failed requests in a row: " +
                  std::string(reason),
              {{"failures", in_a_row}, {"reason", reason}, {"check", "passive"}});
}

void ClientSession::reset_for_next_request() {
    request_parser_.reset();
    response_parser_.reset();
    backend_out_.clear();
    backend_out_sent_ = 0;
    backend_in_.clear();
    config_.reset();
    topology_.reset();
    backend_rt_.reset();
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
    retry_safe_ = false;
    stale_retried_ = false;
    prefer_new_connection_ = false;
    retry_head_.clear();
    backend_bytes_received_ = 0;
    backend_in_use_ = false;
    proxy_error_ = false;
    request_tag_.clear();
    request_head_seen_ = false;
    outcome_recorded_ = false;
    metrics_series_ = 0;
    group_config_ = nullptr;
    sticky_ = nullptr;
    sticky_group_.clear();
    sticky_set_cookie_.clear();
    backend_ready_at_ = {};
    backend_done_at_ = {};
    request_started_ = client_in_.has_data();  // pipelined bytes: the next request has begun
    if (request_started_) request_start_ = Clock::now();

    // Deadlines for the next request: keep-alive idle until its first byte, then the
    // header deadline. Pipelined bytes already buffered start the header deadline now.
    first_request_ = false;
    idle_since_ = Clock::now();
    head_deadline_set_ = client_in_.has_data();
    if (head_deadline_set_) head_deadline_ = idle_since_ + std::chrono::milliseconds(timeouts_.client_header_ms);
}

ClientSession::Deadline ClientSession::desired_deadline(Pending kind, TimePoint* at) const {
    const auto now = Clock::now();
    const auto ms = [](std::uint32_t v) { return std::chrono::milliseconds(v); };
    switch (kind) {
        case Pending::ClientRecv:
            if (request_head_seen_) {
                *at = now + ms(timeouts_.client_body_idle_ms);
                return Deadline::ClientBody;
            }
            if (!first_request_ && !head_deadline_set_) {
                *at = idle_since_ + ms(timeouts_.client_keepalive_idle_ms);
                return Deadline::ClientKeepAlive;
            }
            *at = head_deadline_;
            return Deadline::ClientHeader;
        case Pending::ClientSend:
            *at = now + ms(timeouts_.client_write_idle_ms);
            return Deadline::ClientWrite;
        case Pending::BackendConnect:
            *at = now + ms(timeouts_.backend_connect_ms);
            return Deadline::BackendConnect;
        case Pending::BackendSend:
            *at = now + ms(timeouts_.backend_idle_ms);
            return Deadline::BackendIdle;
        case Pending::BackendRecv:
            if (!response_started_) {
                *at = response_deadline_;
                return Deadline::BackendResponse;
            }
            *at = now + ms(timeouts_.backend_idle_ms);
            return Deadline::BackendIdle;
        case Pending::None:
        case Pending::PoolWait:  // the pool's wait queue has its own timeout
            break;
    }
    return Deadline::None;
}

// Called before every I/O: keeps the matching deadline armed. Absolute deadlines stay put
// across reads; idle deadlines move forward with each I/O.
void ClientSession::update_deadline(Pending kind) {
    TimePoint at{};
    const Deadline wanted = desired_deadline(kind, &at);
    if (wanted == armed_ && (wanted == Deadline::None || at == armed_at_)) return;
    disarm_deadline();
    if (wanted == Deadline::None) return;
    armed_ = wanted;
    armed_at_ = at;
    const std::uint64_t generation = ++deadline_generation_;
    deadline_timer_ = ctx_.timers->schedule(at, [weak = weak_from_this(), generation] {
        if (auto self = weak.lock()) self->on_deadline(generation);
    });
}

void ClientSession::disarm_deadline() noexcept {
    if (deadline_timer_ != 0) ctx_.timers->cancel(deadline_timer_);
    deadline_timer_ = 0;
    armed_ = Deadline::None;
    ++deadline_generation_;  // a callback already running for the old deadline does nothing
}

// Timer thread: never touches request state beyond flagging the expiry; cancelling the
// pending I/O makes its completion arrive on an IOCP worker, which applies the outcome.
void ClientSession::on_deadline(std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex_);
    if (closed_ || generation != deadline_generation_ || armed_ == Deadline::None) return;
    expired_ = armed_;
    armed_ = Deadline::None;
    deadline_timer_ = 0;
    if (pending_ != Pending::None && pending_ != Pending::PoolWait && op_.socket != INVALID_SOCKET) {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op_.socket), &op_.overlapped);
    }
}

// Plan IV.12 / VI: a drain timeout aborts the remaining requests. Not the backend's fault, so
// it counts against neither its health nor its metrics.
void ClientSession::handle_drain_abort() {
    ctx_.counters->drain_aborts.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t timeout = config_ ? config_->timeouts.drain_ms : 0;
    log_event("drain_aborted",
              "request to draining backend " + backend_rt_->id + " aborted: drain timeout (" + std::to_string(timeout) +
                  " ms)" + (response_flushed_ ? "; client connection cut mid-response" : ": 502"),
              {{"timeout_ms", timeout}});
    if (!response_flushed_) {
        trace(TraceStep::TimedOut, 502);
        fail_request(502, /*backend_fault=*/false);
        return;
    }
    trace(TraceStep::Aborted);
    end_backend_use(BackendOutcome::NotJudged);
    record_outcome(0);
    close_all(true);
}

// Plan VI "On expiry" column.
void ClientSession::handle_timeout(Deadline expired) {
    const bool client_side = expired == Deadline::ClientHeader || expired == Deadline::ClientKeepAlive ||
                             expired == Deadline::ClientBody || expired == Deadline::ClientWrite;
    (client_side ? ctx_.counters->client_timeouts : ctx_.counters->backend_timeouts)
        .fetch_add(1, std::memory_order_relaxed);
    const auto client_timeout = [&](const char* name, std::uint32_t ms) {
        log_event("timeout", std::string(name) + " timeout (" + std::to_string(ms) + " ms) from " + peer_ip_,
                  {{"timeout", name}, {"peer", peer_ip_}});
    };
    switch (expired) {
        case Deadline::ClientHeader:  // slowloris defense
            trace(TraceStep::TimedOut, 0);
            client_timeout("client_header", timeouts_.client_header_ms);
            close_all(false);
            break;
        case Deadline::ClientKeepAlive:  // routine: an idle keep-alive connection is closed, not logged
            trace(TraceStep::TimedOut, 0);
            close_all(false);
            break;
        case Deadline::ClientWrite:  // the client stopped reading
            trace(TraceStep::TimedOut, 0);
            client_timeout("client_write_idle", timeouts_.client_write_idle_ms);
            close_all(true);
            break;
        case Deadline::ClientBody:
            trace(TraceStep::TimedOut, 408);
            client_timeout("client_body_idle", timeouts_.client_body_idle_ms);
            fail_request(408, /*backend_fault=*/false);
            break;
        case Deadline::BackendConnect:
            trace(TraceStep::TimedOut, 502);
            fail_request(502, /*backend_fault=*/true,
                         "connect timeout (" + std::to_string(timeouts_.backend_connect_ms) + " ms)");
            break;
        case Deadline::BackendResponse:
            trace(TraceStep::TimedOut, 504);
            fail_request(504, /*backend_fault=*/true,
                         "no response headers within " + std::to_string(timeouts_.backend_response_ms) + " ms");
            break;
        case Deadline::BackendIdle:
            if (stage_ == Stage::Request) {  // the backend stopped taking the request body
                trace(TraceStep::TimedOut, 504);
                fail_request(504, /*backend_fault=*/true,
                             "stalled taking the request for " + std::to_string(timeouts_.backend_idle_ms) + " ms");
            } else {  // the backend stalled mid-response: 502, or cut the client off if bytes went out
                trace(TraceStep::TimedOut, response_flushed_ ? 0 : 502);
                backend_response_failed("stalled mid-response for " + std::to_string(timeouts_.backend_idle_ms) +
                                        " ms");
            }
            break;
        case Deadline::None:
            break;
    }
}

void ClientSession::record_outcome(int status) noexcept {
    if (outcome_recorded_ || request_id_ == 0 || ctx_.metrics == nullptr) return;
    outcome_recorded_ = true;
    const TimePoint now = Clock::now();
    const Duration total = request_started_ ? now - request_start_ : Duration::zero();
    std::optional<Duration> backend_time;
    if (status > 0 && !proxy_error_ && backend_ready_at_ != TimePoint{} && backend_done_at_ >= backend_ready_at_) {
        backend_time = backend_done_at_ - backend_ready_at_;
    }
    ctx_.metrics->record(metrics_series_, now, total, backend_time, metrics::status_class(status));
}

void ClientSession::log_event(std::string_view type, std::string message, nlohmann::json fields) const noexcept {
    if (ctx_.events == nullptr) return;
    try {
        ctx_.events->emit(type, std::move(message), std::move(fields),
                          backend_rt_ ? std::string_view(backend_rt_->id) : std::string_view{}, request_tag_);
    } catch (...) {
        // Logging must never take a request down.
    }
}

void ClientSession::close_backend(bool reusable) noexcept {
    if (holding_slot_ && backend_rt_) {
        if (backend_ != INVALID_SOCKET && backend_connected_) {
            backend_rt_->pool.release(backend_, reusable, Clock::now());  // closes it unless reused
        } else {
            if (backend_ != INVALID_SOCKET) ::closesocket(backend_);
            backend_rt_->pool.abandon_slot();
        }
    } else if (backend_ != INVALID_SOCKET) {
        ::closesocket(backend_);
    }
    backend_ = INVALID_SOCKET;
    backend_connected_ = false;
    holding_slot_ = false;
    reused_connection_ = false;
}

void ClientSession::close_all(bool abortive) noexcept {
    if (!closed_ && request_id_ != 0) record_outcome(0);  // closed mid-request: counted as aborted
    closed_ = true;
    disarm_deadline();
    // Queued for a pool slot: withdraw the ticket and wake ourselves so the pending
    // reference is released through the normal completion path.
    if (pending_ == Pending::PoolWait && ticket_ && backend_rt_) {
        if (backend_rt_->pool.cancel(ticket_)) ticket_->wake();
    }
    close_backend(false);
    end_backend_use(BackendOutcome::NotJudged);
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

void ClientSession::trace(TraceStep step, int status, std::string_view detail) const noexcept {
    if (TraceSink* sink = ctx_.trace->load(std::memory_order_acquire)) {
        sink->on_trace(TraceEvent{request_id_, step, status});
    }
    // Plan IV.18: in debug logging mode every step of every request goes to the event log.
    if (ctx_.events != nullptr && ctx_.events->trace_requests() && request_id_ != 0) {
        std::string message(to_string(step));
        if (status != 0) message += " " + std::to_string(status);
        if (!detail.empty()) message += ": " + std::string(detail);
        log_event("request_step", message, {{"step", to_string(step)}, {"status", status}, {"detail", detail}});
    }
}

}  // namespace lb::proxy
