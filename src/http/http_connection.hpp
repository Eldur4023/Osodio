#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <cstdint>
#include "http_parser.hpp"
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"
#include "../../include/osodio/cancel.hpp"
#include <openssl/ssl.h>

namespace osodio::http {

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    // ssl_ctx: when non-null, the connection performs a TLS handshake before
    // parsing HTTP.  ssl_ctx is not owned; its lifetime must exceed this object.
    HttpConnection(int fd, core::EventLoop& loop, osodio::DispatchFn dispatch,
                   std::shared_ptr<std::atomic<int>> conn_count = nullptr,
                   SSL_CTX* ssl_ctx = nullptr);
    ~HttpConnection();

    void start();
    void on_event(uint32_t events);

private:
    int                fd_;
    core::EventLoop&   loop_;
    osodio::DispatchFn dispatch_;
    std::shared_ptr<std::atomic<int>>          conn_count_;   // decremented on close()
    std::shared_ptr<osodio::CancellationToken> cancel_token_; // one per request
    HttpParser         parser_;
    bool               closed_         = false;

    // ── TLS state ─────────────────────────────────────────────────────────────
    // ssl_ is non-null only when app.tls() was configured.
    // tls_handshaking_ is true from start() until SSL_accept() returns 1.
    // During that window, on_event() routes all events to do_tls_handshake().
    SSL* ssl_              = nullptr;
    bool tls_handshaking_  = false;

    // Weak reference to the current request — used in WebSocket mode to route
    // do_read() bytes into the WS frame parser instead of the HTTP parser.
    std::weak_ptr<osodio::Request> current_req_;

    // ── Response buffer limit ─────────────────────────────────────────────────
    // Hard cap on the size of a single response.  Connections that exceed this
    // are closed to prevent unbounded RAM growth from slow-reading clients.
    static constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024; // 16 MB

    // ── Write buffer ─────────────────────────────────────────────────────────
    // Non-blocking writes: if send buffer is full (EAGAIN), data is queued here
    // and flushed when EPOLLOUT fires.  Using an offset avoids O(n) erases.
    std::string write_buf_;
    size_t      write_offset_ = 0;
    bool        keep_alive_   = false;  // stored here so on_write_complete can act

    // ── Timeouts ──────────────────────────────────────────────────────────────
    // kHeaderTimeoutMs: armed at construction; fires 408 if complete headers are
    //   not received within this window (Slowloris defence).
    //   Cancelled in dispatch() once headers are fully parsed.
    // kRequestTimeoutMs: armed in dispatch(); fires 408 if handler + write take
    //   too long.  Cancelled in on_write_complete().
    static constexpr int kHeaderTimeoutMs  = 5'000;
    static constexpr int kRequestTimeoutMs = 30'000;
    int header_tfd_  = -1;
    int timeout_tfd_ = -1;

    // ── sendfile state ────────────────────────────────────────────────────────
    // When serving static files, we skip the read-into-buffer step and stream
    // directly from the file descriptor to the socket.  The connection sends
    // the HTTP headers via the normal write_buf_ path, then transitions to
    // do_sendfile() once the headers are fully flushed.
    int    file_fd_        = -1;
    off_t  file_offset_    = 0;
    size_t file_remaining_ = 0;

    void do_read();
    void do_write();
    void do_sendfile();
    void do_tls_handshake();
    void on_write_complete();

    // Begin writing `data`; buffers any unsent remainder and arms EPOLLOUT.
    void send_response(std::string data);
    void send_error(int code, const char* msg);
    void close();

    void dispatch(ParsedRequest req);
    void finish_dispatch(osodio::Request& request, osodio::Response& response);
};

} // namespace osodio::http
