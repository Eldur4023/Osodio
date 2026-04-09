#include "http_connection.hpp"
#include "../../include/osodio/request.hpp"
#include "../../include/osodio/response.hpp"
#include "../../include/osodio/task.hpp"

#include <sys/epoll.h>
#include <sys/sendfile.h>
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
                               std::shared_ptr<std::atomic<int>> conn_count)
    : fd_(fd)
    , loop_(loop)
    , dispatch_(std::move(dispatch))
    , conn_count_(std::move(conn_count))
    , parser_([this](ParsedRequest req) { this->dispatch(std::move(req)); })
{
}

void HttpConnection::start() {
    // Arm the header timeout immediately — if we don't receive a complete
    // HTTP request within kHeaderTimeoutMs, close the connection (Slowloris).
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    header_tfd_ = loop_.schedule_timer(kHeaderTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->send_error(408, "Request Header Timeout");
                self->close();
            }
        }
    });
}

HttpConnection::~HttpConnection() {
    if (!closed_) {
        // Safe closure: we skip loop_.remove() to avoid re-entrant erase if
        // this destructor is called during EventLoop callbacks_ teardown.
        if (header_tfd_  >= 0) ::close(header_tfd_);
        if (timeout_tfd_ >= 0) ::close(timeout_tfd_);
        if (file_fd_     >= 0) ::close(file_fd_);
        ::close(fd_);
    }
}

// ── Event dispatch ────────────────────────────────────────────────────────────

void HttpConnection::on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close(); return; }

    // While write_buf_ has data, only EPOLLOUT is armed.
    // Once the buffer is drained, EPOLLIN is re-armed (see on_write_complete).
    if (events & EPOLLOUT) do_write();
    if (events & EPOLLIN)  do_read();
}

// ── Read path ─────────────────────────────────────────────────────────────────

void HttpConnection::do_read() {
    char buf[16384];
    while (!closed_) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n == 0)  { close(); return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            close(); return;
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
    // Determine keep-alive before building the response
    keep_alive_ = (request.version == "HTTP/1.1");
    if (auto conn = request.header("connection")) {
        std::string val = *conn;
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        keep_alive_ = (val != "close");
    }
    response.header("Connection", keep_alive_ ? "keep-alive" : "close");

    // SSE mode: headers were already written directly by make_sse().
    // Just close the connection without sending a second response.
    if (response.sse_started()) {
        keep_alive_ = false;
        close();
        return;
    }

    // sendfile path: open the file and set up state; build() emits only headers
    if (!response.sendfile_path().empty()) {
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
    ssize_t n = ::write(fd_, data.data(), data.size());
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(); return;
        }
        // EAGAIN on first byte — buffer everything, start at offset 0
        write_buf_    = std::move(data);
        write_offset_ = 0;
    } else {
        size_t written = static_cast<size_t>(n);
        if (written == data.size()) {
            on_write_complete();
            return;
        }
        // Partial write — keep the full string and advance the offset.
        // Avoids an O(n) substr copy; do_write() reads from write_offset_.
        write_buf_    = std::move(data);
        write_offset_ = written;
    }

    // Arm EPOLLOUT only; drop EPOLLIN until write completes
    loop_.modify(fd_, EPOLLOUT);
}

void HttpConnection::do_write() {
    while (write_offset_ < write_buf_.size()) {
        ssize_t n = ::write(fd_,
                            write_buf_.data()  + write_offset_,
                            write_buf_.size()  - write_offset_);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // try next EPOLLOUT
            if (errno == EINTR) continue;
            close(); return;
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
    while (file_remaining_ > 0) {
        ssize_t n = ::sendfile(fd_, file_fd_, &file_offset_,
                               std::min(file_remaining_, size_t{256 * 1024}));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return; // wait for EPOLLOUT
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

    // Signal any suspended coroutines (e.g. co_await sleep()) to wake up and
    // exit rather than waiting out their full timer duration.
    if (cancel_token_) cancel_token_->cancel();

    loop_.cancel_timer(header_tfd_);
    header_tfd_ = -1;
    loop_.cancel_timer(timeout_tfd_);
    timeout_tfd_ = -1;
    loop_.remove(fd_);
    ::close(fd_);
    if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
    if (conn_count_) conn_count_->fetch_sub(1, std::memory_order_relaxed);
}

} // namespace osodio::http
