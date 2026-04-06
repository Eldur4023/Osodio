#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include "types.hpp"

namespace osodio {

struct RouteMatch {
    bool    found   = false;
    Handler handler = nullptr;
    std::unordered_map<std::string, std::string> params;
};

class Router {
public:
    void add(std::string method, std::string pattern, Handler handler);

    RouteMatch match(const std::string& method, const std::string& path) const;

private:
    struct Route {
        std::string method;   // uppercase, e.g. "GET"
        std::string pattern;  // normalized, e.g. "/users/:id"
        Handler     handler;
    };

    std::vector<Route> routes_;

    // Converts {id} → :id  (supports both FastAPI and Express styles)
    static std::string normalize_pattern(const std::string& p);

    // Returns true if pattern matches path, filling params
    static bool match_path(
        const std::string& pattern,
        const std::string& path,
        std::unordered_map<std::string, std::string>& params);
};

} // namespace osodio
