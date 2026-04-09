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

    // Raw socket fd — used by res.sse(req) to write SSE headers and stream
    // events directly.  Not intended for general handler use.
    int _conn_fd = -1;

    // Pointer to the service container (set by App::run before dispatch).
    // Non-owning: the App owns the container and outlives all requests.
    class ServiceContainer* container = nullptr;

    // Cancellation token — shared with the HttpConnection.
    // Cancelled when the connection closes (timeout, disconnect, write error).
    // Check in long-running handlers to exit early.
    std::shared_ptr<CancellationToken> cancel_token;

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
