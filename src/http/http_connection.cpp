#include "http_connection.hpp"
#ifdef OSODIO_HAS_HTTP2
#  include "http2_connection.hpp"
#endif
#include "../../include/osodio/request.hpp"
#include "../../include/osodio/response.hpp"
#include "../../include/osodio/task.hpp"
#include "../../include/osodio/metrics.hpp"

#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace osodio::http {

// ── URL helpers ───────────────────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char buf[3] = {s[i+1], s[i+2], '\0'};
            out += static_cast<char>(std::strtoul(buf, nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static void parse_query(const std::string& qs,
                        std::unordered_map<std::string, std::string>& out) {
    if (qs.empty()) return;
    std::istringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos)
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        else if (!pair.empty())
            out[url_decode(pair)] = "";
    }
}

// ── HttpConnection ────────────────────────────────────────────────────────────

HttpConnection::HttpConnection(int fd, core::EventLoop& loop,
                               osodio::DispatchFn dispatch,
                               std::shared_ptr<std::atomic<int>> conn_count,
                               SSL_CTX* ssl_ctx)
    : fd_(fd)
    , loop_(loop)
    , dispatch_(std::move(dispatch))
    , conn_count_(std::move(conn_count))
    , parser_([this](ParsedRequest req) { this->dispatch(std::move(req)); })
{
#ifndef OSODIO_HAS_TLS
    (void)ssl_ctx;
#else
    if (ssl_ctx) {
        ssl_ = SSL_new(ssl_ctx);
        if (!ssl_) return; // close() will be called in start() → error path
        SSL_set_fd(ssl_, fd_);
        SSL_set_accept_state(ssl_);
    }
#endif // OSODIO_HAS_TLS
}

void HttpConnection::start() {
    // Arm the header timeout — covers both TLS handshake + HTTP header receive.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    header_tfd_ = loop_.schedule_timer(kHeaderTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->send_error(408, "Request Header Timeout");
                self->close();
            }
        }
    });

    // TLS: wait for first EPOLLIN before starting the handshake.
    // (The fd is added to epoll by TcpServer immediately after start().)
#ifdef OSODIO_HAS_TLS
    if (ssl_) tls_handshaking_ = true;
#endif
}

HttpConnection::~HttpConnection() {
    if (!closed_) {
        // Safe closure: we skip loop_.remove() to avoid re-entrant erase if
        // this destructor is called during EventLoop callbacks_ teardown.
        if (header_tfd_  >= 0) ::close(header_tfd_);
        if (timeout_tfd_ >= 0) ::close(timeout_tfd_);
        if (file_fd_     >= 0) ::close(file_fd_);
#ifdef OSODIO_HAS_TLS
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
#endif
        ::close(fd_);
    }
}

// ── Event dispatch ────────────────────────────────────────────────────────────

void HttpConnection::on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close(); return; }

    // TLS handshake: route ALL events until SSL_accept() completes.
#ifdef OSODIO_HAS_TLS
    if (tls_handshaking_) { do_tls_handshake(); return; }
#endif

    // While write_buf_ has data, only EPOLLOUT is armed.
    // Once the buffer is drained, EPOLLIN is re-armed (see on_write_complete).
    if (events & EPOLLOUT) do_write();
    if (events & EPOLLIN)  do_read();
}

// ── TLS handshake ─────────────────────────────────────────────────────────────
//
// Called for every epoll event while tls_handshaking_ is true.
// SSL_accept() is non-blocking: it may need multiple EPOLLIN/EPOLLOUT rounds
// before the handshake finishes.

#ifdef OSODIO_HAS_TLS
void HttpConnection::do_tls_handshake() {
    int r = SSL_accept(ssl_);
    if (r == 1) {
        // Handshake complete — check ALPN-negotiated protocol.
        const unsigned char* proto     = nullptr;
        unsigned int         proto_len = 0;
        SSL_get0_alpn_selected(ssl_, &proto, &proto_len);

#ifdef OSODIO_HAS_HTTP2
        if (proto_len == 2 && memcmp(proto, "h2", 2) == 0) {
            // ── Upgrade to HTTP/2 ──────────────────────────────────────────
            // Transfer fd and ssl* ownership to Http2Connection.
            // The EventLoop copies the callback before calling it (see run()),
            // so loop_.remove() + loop_.add() here are safe.
            int  xfd  = fd_;
            SSL* xssl = ssl_;
            fd_   = -1;    // prevent close() from touching these
            ssl_  = nullptr;
            closed_ = true; // suppress any further activity on this object
            loop_.cancel_timer(header_tfd_);
            header_tfd_ = -1;

            loop_.remove(xfd);

            auto h2 = std::make_shared<Http2Connection>(
                xfd, xssl, loop_, dispatch_, conn_count_);

            if (!h2->init()) return; // Http2Connection::init() cleaned up on failure

            loop_.add(xfd, EPOLLIN | EPOLLOUT,
                      [h2](uint32_t ev) { h2->on_event(ev); });
            return;
        }
#endif // OSODIO_HAS_HTTP2

        // HTTP/1.1 (or no ALPN) — proceed normally.
        tls_handshaking_ = false;
        loop_.modify(fd_, EPOLLIN);
        return;
    }
    int err = SSL_get_error(ssl_, r);
    if (err == SSL_ERROR_WANT_READ) {
        loop_.modify(fd_, EPOLLIN);   // already default, but be explicit
    } else if (err == SSL_ERROR_WANT_WRITE) {
        loop_.modify(fd_, EPOLLOUT);  // need to flush handshake data
    } else {
        // Fatal TLS error (bad client hello, cert mismatch, etc.)
        close();
    }
}
#endif // OSODIO_HAS_TLS

// ── Read path ─────────────────────────────────────────────────────────────────

void HttpConnection::do_read() {
    // WebSocket mode: delegate all reading to the WS frame parser.
    if (auto req = current_req_.lock(); req && req->_ws_on_readable) {
        req->_ws_on_readable();
        return;
    }

    char buf[16384];
    while (!closed_) {
        ssize_t n;
#ifdef OSODIO_HAS_TLS
        if (ssl_) {
            n = SSL_read(ssl_, buf, sizeof(buf));
            if (n <= 0) {
                int err = SSL_get_error(ssl_, static_cast<int>(n));
                if (err == SSL_ERROR_WANT_READ)  return; // wait for more data
                if (err == SSL_ERROR_ZERO_RETURN) { close(); return; } // clean shutdown
                if (err == SSL_ERROR_WANT_WRITE)  return; // rare renegotiation case
                close(); return;
            }
        } else
#endif
        {
            n = ::read(fd_, buf, sizeof(buf));
            if (n == 0) { close(); return; }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                close(); return;
            }
        }
        if (!parser_.feed(buf, static_cast<size_t>(n))) {
            send_error(400, "Bad Request");
            close();
            return;
        }
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void HttpConnection::dispatch(ParsedRequest req_parsed) {
    // Fresh cancellation token for this request.
    cancel_token_ = std::make_shared<osodio::CancellationToken>();

    // Set thread-locals so handlers can call sleep(ms) without explicit args.
    osodio::detail::current_loop  = &loop_;
    osodio::detail::current_token = cancel_token_;

    auto req_ptr = std::make_shared<osodio::Request>();
    auto res_ptr = std::make_shared<osodio::Response>();

    req_ptr->method       = req_parsed.method;
    req_ptr->path         = req_parsed.path;
    req_ptr->version      = req_parsed.version;
    req_ptr->headers      = std::move(req_parsed.headers);
    req_ptr->body         = std::move(req_parsed.body);
    req_ptr->loop         = &loop_;
    req_ptr->cancel_token = cancel_token_;
    req_ptr->_conn_fd     = fd_;
    parse_query(req_parsed.query, req_ptr->query);

    // Keep a weak ref for WebSocket mode — do_read() routes through it.
    current_req_ = req_ptr;

    // Resolve remote IP from the socket
    sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
        char ipbuf[INET6_ADDRSTRLEN]{};
        if (ss.ss_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&ss)->sin_addr,
                      ipbuf, sizeof(ipbuf));
        } else if (ss.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr,
                      ipbuf, sizeof(ipbuf));
        }
        req_ptr->remote_ip = ipbuf;
    }

    // Headers fully received — cancel the Slowloris timer.
    loop_.cancel_timer(header_tfd_);
    header_tfd_ = -1;

    // ── Arm request timeout ───────────────────────────────────────────────────
    // If the handler + write don't complete within kRequestTimeoutMs, send 408.
    // cancel_timer() is called in on_write_complete() when everything succeeds.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    timeout_tfd_ = loop_.schedule_timer(kRequestTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->send_error(408, "Request Timeout");
                self->close();
            }
        }
    });

    auto wrapper_task = [](std::shared_ptr<osodio::Request> req_ptr,
                           std::shared_ptr<osodio::Response> res_ptr,
                           osodio::DispatchFn disp) -> osodio::Task<void> {
        try {
            co_await disp(*req_ptr, *res_ptr);
        } catch (const std::exception& e) {
            res_ptr->status(500).json({{"error", e.what()}});
        } catch (...) {
            res_ptr->status(500).json({{"error", "Internal Server Error"}});
        }
    }(req_ptr, res_ptr, dispatch_);

    auto h = wrapper_task.detach();
    h.promise().loop = &loop_;

    auto self = shared_from_this();
    h.promise().on_complete = [self, req_ptr, res_ptr]() {
        self->finish_dispatch(*req_ptr, *res_ptr);
    };

    h.resume();
}

void HttpConnection::finish_dispatch(osodio::Request& request,
                                     osodio::Response& response) {
    // Record the request in the global metrics counter.
    osodio::Metrics::instance().record(response.status_code());

    // Determine keep-alive before building the response
    keep_alive_ = (request.version == "HTTP/1.1");
    if (auto conn = request.header("connection")) {
        std::string val = *conn;
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        keep_alive_ = (val != "close");
    }
    response.header("Connection", keep_alive_ ? "keep-alive" : "close");

    // SSE / WebSocket mode: headers were already written directly.
    // Just close the connection — do not send a second response.
    if (response.sse_started() || response.ws_started()) {
        keep_alive_ = false;
        close();
        return;
    }

    // sendfile path
    if (!response.sendfile_path().empty()) {
#ifdef OSODIO_HAS_TLS
        if (ssl_) {
            // TLS: sendfile(2) bypasses OpenSSL — must read file into userspace.
            // response.build() emits headers with the correct Content-Length;
            // we append the raw file bytes to form a complete HTTP response.
            std::ifstream f(response.sendfile_path(), std::ios::binary);
            if (!f) { send_error(500, "Cannot open file"); return; }
            std::string file_body(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            send_response(response.build() + file_body);
            return;
        }
#endif
        // Non-TLS: open the file; do_sendfile() will stream it via sendfile(2).
        int fd = ::open(response.sendfile_path().c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            osodio::Response err;
            err.status(500).json({{"error", "Cannot open file"}});
            err.header("Connection", "close");
            keep_alive_ = false;
            send_response(err.build());
            return;
        }
        file_fd_        = fd;
        file_offset_    = 0;
        file_remaining_ = static_cast<size_t>(response.sendfile_size());
    }

    send_response(response.build());
}

// ── Write path ────────────────────────────────────────────────────────────────
//
// Strategy:
//   1. Try an immediate write(2).  If all bytes go through → done.
//   2. If EAGAIN or partial write → buffer the remainder, arm EPOLLOUT.
//      EPOLLIN is removed while writing so we don't try to parse a new
//      request before the current response is fully sent.
//   3. EPOLLOUT fires → do_write() drains the buffer.
//   4. Buffer empty → on_write_complete(): cancel timeout, handle keep-alive.

void HttpConnection::send_response(std::string data) {
    if (closed_) return;

    // Hard cap: if a single response exceeds kMaxResponseBytes, the client is
    // reading so slowly that buffering would exhaust RAM.  Close cleanly.
    if (data.size() > kMaxResponseBytes) {
        keep_alive_ = false;
        close();
        return;
    }

    // Try immediate write
    ssize_t n;
#ifdef OSODIO_HAS_TLS
    if (ssl_) {
        n = SSL_write(ssl_, data.data(), static_cast<int>(data.size()));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, static_cast<int>(n));
            if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
                close(); return;
            }
            write_buf_    = std::move(data);
            write_offset_ = 0;
            loop_.modify(fd_, EPOLLOUT);
            return;
        }
    } else
#endif
    {
        n = ::write(fd_, data.data(), data.size());
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(); return;
            }
            write_buf_    = std::move(data);
            write_offset_ = 0;
            loop_.modify(fd_, EPOLLOUT);
            return;
        }
    }

    {
        size_t written = static_cast<size_t>(n);
        if (written == data.size()) {
            on_write_complete();
            return;
        }
        // Partial write — keep the full string and advance the offset.
        write_buf_    = std::move(data);
        write_offset_ = written;
    }

    // Arm EPOLLOUT only; drop EPOLLIN until write completes
    loop_.modify(fd_, EPOLLOUT);
}

void HttpConnection::do_write() {
    while (write_offset_ < write_buf_.size()) {
        ssize_t n;
#ifdef OSODIO_HAS_TLS
        if (ssl_) {
            n = SSL_write(ssl_,
                          write_buf_.data()  + write_offset_,
                          static_cast<int>(write_buf_.size() - write_offset_));
            if (n <= 0) {
                int err = SSL_get_error(ssl_, static_cast<int>(n));
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) return;
                close(); return;
            }
        } else
#endif
        {
            n = ::write(fd_,
                        write_buf_.data()  + write_offset_,
                        write_buf_.size()  - write_offset_);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                close(); return;
            }
        }
        write_offset_ += static_cast<size_t>(n);
    }

    // All header/body bytes sent — reset buffer
    write_buf_.clear();
    write_offset_ = 0;

    // If a file is pending, stream it via sendfile(2)
    if (file_fd_ >= 0) { do_sendfile(); return; }

    on_write_complete();
}

void HttpConnection::do_sendfile() {
    // Only reachable when ssl_ is null (TLS path reads the file in finish_dispatch).
    while (file_remaining_ > 0) {
        ssize_t n = ::sendfile(fd_, file_fd_, &file_offset_,
                               std::min(file_remaining_, size_t{256 * 1024}));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            close(); return;
        }
        if (n == 0) break; // EOF
        file_remaining_ -= static_cast<size_t>(n);
    }

    ::close(file_fd_);
    file_fd_        = -1;
    file_offset_    = 0;
    file_remaining_ = 0;
    on_write_complete();
}

void HttpConnection::on_write_complete() {
    // Cancel the request timeout — response delivered successfully
    loop_.cancel_timer(timeout_tfd_);
    timeout_tfd_ = -1;

    if (!keep_alive_) {
        close();
        return;
    }

    // Re-arm the header timeout for the next pipelined/keep-alive request.
    // Without this, a client that sends headers slowly on the second request
    // (Slowloris) would go unchecked — the 5s timer only ran for the first one.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    header_tfd_ = loop_.schedule_timer(kHeaderTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->send_error(408, "Request Header Timeout");
                self->close();
            }
        }
    });

    // Ready for the next request
    loop_.modify(fd_, EPOLLIN);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void HttpConnection::send_error(int code, const char* msg) {
    osodio::Response r;
    r.status(code).json({{"error", msg}});
    r.header("Connection", "close");
    // send_error is only called for protocol-level errors; ignore keep-alive
    keep_alive_ = false;
    send_response(r.build());
}

void HttpConnection::close() {
    if (closed_) return;
    closed_ = true;

    // Signal any suspended coroutines (sleep, ws.recv()) to wake up and exit.
    if (cancel_token_) cancel_token_->cancel();

    // Wake any suspended ws.recv() awaitable directly.
    if (auto req = current_req_.lock(); req && req->_ws_on_readable) {
        // _ws_on_readable holds a shared_ptr to WSState; access it via the
        // lambda's closure.  We notify closed via cancel_token (already done
        // above via set_wake), but also call notify_closed in case recv_waiter
        // was registered after the last set_wake.
        req->_ws_on_readable = nullptr;  // prevent further calls after close
    }
    current_req_.reset();

    loop_.cancel_timer(header_tfd_);
    header_tfd_ = -1;
    loop_.cancel_timer(timeout_tfd_);
    timeout_tfd_ = -1;
    loop_.remove(fd_);

    // TLS shutdown: best-effort (non-blocking); we close the fd regardless.
#ifdef OSODIO_HAS_TLS
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
#endif

    ::close(fd_);
    if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
    if (conn_count_) conn_count_->fetch_sub(1, std::memory_order_relaxed);
}

} // namespace osodio::http
