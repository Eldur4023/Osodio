#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <algorithm>
#include <osodio/core/event_loop.hpp>
#include "cancel.hpp"

namespace osodio {

class Request {
public:
    std::string method;
    std::string path;
    std::string version;
    std::string body;
    std::string remote_ip;  // IPv4/IPv6 of the connected peer

    // Headers stored with lowercase keys
    std::unordered_map<std::string, std::string> headers;

    // Path params extracted by the router (e.g. /users/:id → params["id"])
    std::unordered_map<std::string, std::string> params;

    // Query string params (e.g. ?page=1&limit=20 → query["page"] = "1")
    std::unordered_map<std::string, std::string> query;

    // Pointer to the event loop for scheduling tasks
    core::EventLoop* loop = nullptr;

    // Raw socket fd — used by res.sse(req) and WebSocket upgrade.
    int _conn_fd = -1;

    // WebSocket mode: called by HttpConnection::do_read() to feed incoming
    // bytes into the WSState frame parser.  Set by the ws() upgrade wrapper.
    std::function<void()> _ws_on_readable;

    // Pointer to the service container (set by App::run before dispatch).
    // Non-owning: the App owns the container and outlives all requests.
    class ServiceContainer* container = nullptr;

    // Cancellation token — shared with the HttpConnection.
    // Cancelled when the connection closes (timeout, disconnect, write error).
    // Check in long-running handlers to exit early.
    std::shared_ptr<CancellationToken> cancel_token;

    // ── HTTP/2 WebSocket context (RFC 8441) ───────────────────────────────────
    // Set by Http2Connection::dispatch_stream() for CONNECT+websocket streams.
    // app.ws() checks this to choose the H2 path over the HTTP/1.1 101 path.
    struct H2WSContext {
        // Called once with the incoming-data feed callback.  Sends 200 HEADERS
        // (no END_STREAM) and registers the feed callback so that incoming DATA
        // frames are forwarded to the WSState parser.
        std::function<void(std::function<void(const uint8_t*, size_t)>)> begin;
        // Enqueue WS frame bytes to be sent as a DATA chunk.
        std::function<void(std::string)> push;
        // Send DATA+END_STREAM when the handler exits.
        std::function<void()>            close_stream;
    };
    std::shared_ptr<H2WSContext> _h2_ws_ctx;

    // ── HTTP/2 SSE context ────────────────────────────────────────────────────
    // Set by Http2Connection::dispatch_stream() on h2 streams.
    // make_sse() checks this to choose between the raw-fd path (HTTP/1.1) and
    // the nghttp2 DATA-frame path (HTTP/2).
    struct H2SSEContext {
        std::function<void()>           begin;  // submit HEADERS frame, set up provider
        std::function<void(std::string)> push;  // enqueue a DATA chunk
        std::function<void()>           end;    // send DATA+END_STREAM when handler exits
    };
    std::shared_ptr<H2SSEContext> _h2_sse_ctx;

    // Convenience: true if the underlying connection has been closed.
    bool is_cancelled() const noexcept {
        return cancel_token && cancel_token->is_cancelled();
    }

    // Convenience: get a header by name (case-insensitive)
    std::optional<std::string> header(std::string name) const {
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        auto it = headers.find(name);
        if (it == headers.end()) return std::nullopt;
        return it->second;
    }

    // Convenience: get a query param with a default
    std::string query_param(const std::string& key, const std::string& def = "") const {
        auto it = query.find(key);
        return (it != query.end()) ? it->second : def;
    }
};

} // namespace osodio
