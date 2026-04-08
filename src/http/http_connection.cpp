#include "http_connection.hpp"
#include "../../include/osodio/request.hpp"
#include "../../include/osodio/response.hpp"
#include "../../include/osodio/task.hpp"

#include <sys/epoll.h>
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
                               osodio::DispatchFn dispatch)
    : fd_(fd)
    , loop_(loop)
    , dispatch_(std::move(dispatch))
    , parser_([this](ParsedRequest req) { this->dispatch(std::move(req)); })
{}

HttpConnection::~HttpConnection() {
    if (!closed_) {
        // Safe closure: we skip loop_.remove() to avoid re-entrant erase if
        // this destructor is called during EventLoop callbacks_ teardown.
        if (timeout_tfd_ >= 0) ::close(timeout_tfd_);
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
    // Set the thread-local loop so handlers can call sleep(ms) without req.loop.
    osodio::detail::current_loop = &loop_;

    auto req_ptr = std::make_shared<osodio::Request>();
    auto res_ptr = std::make_shared<osodio::Response>();

    req_ptr->method  = req_parsed.method;
    req_ptr->path    = req_parsed.path;
    req_ptr->version = req_parsed.version;
    req_ptr->headers = std::move(req_parsed.headers);
    req_ptr->body    = std::move(req_parsed.body);
    req_ptr->loop    = &loop_;
    parse_query(req_parsed.query, req_ptr->query);

    // ── Arm request timeout ───────────────────────────────────────────────────
    // If the handler + write don't complete within kTimeoutMs, send 408 and close.
    // cancel_timer() is called in on_write_complete() when everything succeeds.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    timeout_tfd_ = loop_.schedule_timer(kTimeoutMs, [self_weak]() {
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

    // Try immediate write
    ssize_t n = ::write(fd_, data.data(), data.size());
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(); return;
        }
        // EAGAIN on first byte — buffer everything
        write_buf_    = std::move(data);
        write_offset_ = 0;
    } else {
        size_t written = static_cast<size_t>(n);
        if (written == data.size()) {
            on_write_complete();
            return;
        }
        // Partial write — buffer the rest
        write_buf_    = data.substr(written);
        write_offset_ = 0;
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

    // All bytes sent — reset buffer
    write_buf_.clear();
    write_offset_ = 0;
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
    // Re-arm for the next request on this connection
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
    loop_.cancel_timer(timeout_tfd_);
    timeout_tfd_ = -1;
    loop_.remove(fd_);
    ::close(fd_);
}

} // namespace osodio::http
