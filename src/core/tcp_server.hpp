#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"

namespace osodio::core {

class TcpServer {
public:
    // max_connections: maximum simultaneous open connections (default 10 000).
    // Excess connections are immediately closed with 503.
    //
    // conn_count: optional shared counter — pass the same one to every
    // TcpServer instance so the limit is enforced globally across all threads.
    // If nullptr, a per-instance counter is created (single-thread behaviour).
    TcpServer(const std::string& host, uint16_t port,
              EventLoop& loop, osodio::DispatchFn dispatch,
              int max_connections = 10'000,
              std::shared_ptr<std::atomic<int>> conn_count = nullptr);
    ~TcpServer();

    // Stop accepting new connections without shutting down the event loop.
    // In-flight connections continue until they finish or are timed out.
    void stop_accepting();

private:
    int                listen_fd_ = -1;
    EventLoop&         loop_;
    osodio::DispatchFn dispatch_;
    int                max_connections_;

    // Shared between TcpServer and every HttpConnection so they can decrement
    // the counter on close without holding a pointer back to TcpServer.
    std::shared_ptr<std::atomic<int>> conn_count_;

    void on_accept();
};

} // namespace osodio::core
