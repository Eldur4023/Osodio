#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <osodio/core/event_loop.hpp>

namespace osodio {

class Request {
public:
    std::string method;
    std::string path;
    std::string version;
    std::string body;

    // Headers stored with lowercase keys
    std::unordered_map<std::string, std::string> headers;

    // Path params extracted by the router (e.g. /users/:id → params["id"])
    std::unordered_map<std::string, std::string> params;

    // Query string params (e.g. ?page=1&limit=20 → query["page"] = "1")
    std::unordered_map<std::string, std::string> query;

    // Pointer to the event loop for scheduling tasks
    core::EventLoop* loop = nullptr;

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
