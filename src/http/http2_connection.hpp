#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"
#include "../../include/osodio/cancel.hpp"
#include "../../include/osodio/response.hpp"

namespace osodio::http {

// ── Http2Connection ───────────────────────────────────────────────────────────
//
// Handles a single TLS+HTTP/2 connection.  Created by HttpConnection when the
// ALPN-negotiated protocol is "h2".  The fd and SSL* are transferred; this
// class takes ownership of both.
//
// Multiple concurrent streams are demultiplexed by nghttp2.  Each stream is
// dispatched as an independent Request/Response pair via the same DispatchFn
// used for HTTP/1.1.

class Http2Connection : public std::enable_shared_from_this<Http2Connection> {
public:
    Http2Connection(int fd, SSL* ssl, core::EventLoop& loop,
                    osodio::DispatchFn dispatch,
                    std::shared_ptr<std::atomic<int>> conn_count);
    ~Http2Connection();

    // Initialise the nghttp2 session and send the server SETTINGS frame.
    // Returns false (and cleans up fd/ssl) if initialisation fails.
    bool init();

    void on_event(uint32_t events);

private:
    int                fd_;
    SSL*               ssl_          = nullptr;
    core::EventLoop&   loop_;
    osodio::DispatchFn dispatch_;
    std::shared_ptr<std::atomic<int>>   conn_count_;
    nghttp2_session*   session_      = nullptr;
    bool               closed_       = false;

    std::string  write_buf_;
    size_t       write_offset_ = 0;

    // ── Per-stream state ──────────────────────────────────────────────────────
    struct BodySrc {
        std::string data;
        size_t      offset = 0;
    };

    struct Stream {
        // Request fields accumulated from HEADERS frame
        std::string                                      method;
        std::string                                      path;     // includes query
        std::string                                      authority;
        std::unordered_map<std::string, std::string>     req_headers;
        std::string                                      body;     // request body
        bool                                             dispatched = false;

        // Cancellation — one token per stream so each coroutine can be
        // independently cancelled on connection close.
        std::shared_ptr<osodio::CancellationToken>       cancel_token;

        // Response body provider for normal responses (owned here so it
        // outlives nghttp2 callbacks)
        std::unique_ptr<BodySrc>                         body_src;

        // ── SSE streaming fields ──────────────────────────────────────────────
        // Populated after the handler calls make_sse(); data provider reads
        // from this queue and returns NGHTTP2_ERR_DEFERRED when it is empty.
        bool                     sse_mode     = false;
        std::vector<std::string> sse_pending;          // chunks awaiting send
        bool                     sse_deferred = false; // provider returned DEFERRED
        bool                     sse_ended    = false; // SSEWriter dtor was called

        // ── WebSocket streaming fields (RFC 8441) ─────────────────────────────
        // Set when the stream is a CONNECT+websocket upgrade.
        bool                     ws_protocol  = false; // :protocol: websocket seen
        bool                     ws_mode      = false; // begin() called
        std::vector<std::string> ws_pending;           // outgoing WS frame bytes
        bool                     ws_deferred  = false;
        bool                     ws_ended     = false;
        // Feed callback — routes incoming DATA to WSState::feed()
        std::function<void(const uint8_t*, size_t)> ws_data_feed;
    };

    std::unordered_map<int32_t, Stream> streams_;

    // ── Internal helpers ──────────────────────────────────────────────────────
    void do_read();
    void do_write();
    void flush();   // nghttp2_session_send() → fills write_buf_ → do_write()
    void close();

    void dispatch_stream(int32_t stream_id, Stream& s);
    void finish_stream (int32_t stream_id, const osodio::Response& res);

    // ── SSE over HTTP/2 helpers ───────────────────────────────────────────────
    // Called via H2SSEContext closures from make_sse() / SSEWriter.
    // All three run on the event loop thread (same thread that owns nghttp2).
    void h2_begin_sse(int32_t stream_id, const osodio::Response& res);
    void h2_push_sse (int32_t stream_id, std::string chunk);
    void h2_end_sse  (int32_t stream_id);

    // ── WebSocket over HTTP/2 helpers (RFC 8441) ─────────────────────────────
    // begin_ws registers the incoming-data feed callback and sends 200 HEADERS.
    void h2_begin_ws(int32_t stream_id,
                     std::function<void(const uint8_t*, size_t)> feed_cb);
    void h2_push_ws (int32_t stream_id, std::string frame);
    void h2_end_ws  (int32_t stream_id);

    // ── nghttp2 callbacks (static; dispatch via user_data → this) ────────────
    static ssize_t send_cb            (nghttp2_session*, const uint8_t*, size_t, int, void*);
    static int     on_begin_headers_cb(nghttp2_session*, const nghttp2_frame*, void*);
    static int     on_header_cb       (nghttp2_session*, const nghttp2_frame*,
                                       const uint8_t*, size_t,
                                       const uint8_t*, size_t,
                                       uint8_t, void*);
    static int     on_frame_recv_cb   (nghttp2_session*, const nghttp2_frame*, void*);
    static int     on_data_chunk_recv_cb(nghttp2_session*, uint8_t, int32_t,
                                         const uint8_t*, size_t, void*);
    static int     on_stream_close_cb (nghttp2_session*, int32_t, uint32_t, void*);
};

} // namespace osodio::http
