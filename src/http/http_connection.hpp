#pragma once
#include <string>
#include <memory>
#include <cstdint>
#include "http_parser.hpp"
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"

namespace osodio::http {

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(int fd, core::EventLoop& loop, osodio::DispatchFn dispatch);
    ~HttpConnection();

    void on_event(uint32_t events);

private:
    int                fd_;
    core::EventLoop&   loop_;
    osodio::DispatchFn dispatch_;
    HttpParser         parser_;
    bool               closed_       = false;

    // ── Write buffer ─────────────────────────────────────────────────────────
    // Non-blocking writes: if send buffer is full (EAGAIN), data is queued here
    // and flushed when EPOLLOUT fires.  Using an offset avoids O(n) erases.
    std::string write_buf_;
    size_t      write_offset_ = 0;
    bool        keep_alive_   = false;  // stored here so on_write_complete can act

    // ── Request timeout ───────────────────────────────────────────────────────
    // Timer armed when dispatch() starts; cancelled in on_write_complete().
    // Fires 408 if the handler + response write take longer than kTimeoutMs.
    static constexpr int kTimeoutMs = 30'000;
    int timeout_tfd_ = -1;

    void do_read();
    void do_write();
    void on_write_complete();

    // Begin writing `data`; buffers any unsent remainder and arms EPOLLOUT.
    void send_response(std::string data);
    void send_error(int code, const char* msg);
    void close();

    void dispatch(ParsedRequest req);
    void finish_dispatch(osodio::Request& request, osodio::Response& response);
};

} // namespace osodio::http
