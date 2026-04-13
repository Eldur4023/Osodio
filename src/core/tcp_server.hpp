#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"
#ifdef OSODIO_HAS_TLS
#  include <openssl/ssl.h>
#else
   struct ssl_ctx_st; typedef struct ssl_ctx_st SSL_CTX;  // incomplete forward decl
#endif

namespace osodio::core {

class TcpServer {
public:
    // max_connections: maximum simultaneous open connections (default 10 000).
    // Excess connections are immediately closed with 503.
    //
    // conn_count: optional shared counter — pass the same one to every
    // TcpServer instance so the limit is enforced globally across all threads.
    // If nullptr, a per-instance counter is created (single-thread behaviour).
    //
    // ssl_ctx: optional TLS context — when non-null every accepted connection
    // performs a TLS handshake before the HTTP parser sees any data.
    TcpServer(const std::string& host, uint16_t port,
              EventLoop& loop, osodio::DispatchFn dispatch,
              int max_connections = 10'000,
              std::shared_ptr<std::atomic<int>> conn_count = nullptr,
              SSL_CTX* ssl_ctx = nullptr);
    ~TcpServer();

    // Stop accepting new connections without shutting down the event loop.
    // In-flight connections continue until they finish or are timed out.
    void stop_accepting();

private:
    int                listen_fd_ = -1;
    EventLoop&         loop_;
    osodio::DispatchFn dispatch_;
    int                max_connections_;
    SSL_CTX*           ssl_ctx_  = nullptr;  // not owned; lifetime ≥ TcpServer

    // Shared between TcpServer and every HttpConnection so they can decrement
    // the counter on close without holding a pointer back to TcpServer.
    std::shared_ptr<std::atomic<int>> conn_count_;

    void on_accept();
};

} // namespace osodio::core
