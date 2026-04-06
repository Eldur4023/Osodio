#include "../include/osodio/router.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace osodio {

// ─────────────────────────────────────────────────────────────────────────────
// Pattern normalization
// ─────────────────────────────────────────────────────────────────────────────

// Converts FastAPI/Flask-style {param} to Express-style :param.
// "/users/{id}/posts/{post_id}" → "/users/:id/posts/:post_id"
std::string Router::normalize_pattern(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '{') {
            out += ':';
            ++i;
            while (i < p.size() && p[i] != '}') out += p[i++];
        } else {
            out += p[i];
        }
    }
    // Remove trailing slash (except root "/")
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Path segment matching
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (!seg.empty()) parts.push_back(seg);
    }
    return parts;
}

bool Router::match_path(const std::string& pattern,
                         const std::string& path,
                         std::unordered_map<std::string, std::string>& params) {
    auto p_segs = split_path(pattern);
    auto r_segs = split_path(path);

    // Wildcard pattern ("*") at the last segment matches everything
    bool has_wildcard = !p_segs.empty() && p_segs.back() == "*";

    if (!has_wildcard && p_segs.size() != r_segs.size()) return false;
    if (has_wildcard  && r_segs.size() < p_segs.size() - 1) return false;

    std::unordered_map<std::string, std::string> captured;
    size_t limit = has_wildcard ? p_segs.size() - 1 : p_segs.size();

    for (size_t i = 0; i < limit; ++i) {
        if (p_segs[i][0] == ':') {
            captured[p_segs[i].substr(1)] = r_segs[i];
        } else if (p_segs[i] != r_segs[i]) {
            return false;
        }
    }

    params = std::move(captured);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void Router::add(std::string method, std::string pattern, Handler handler) {
    // Normalize method to uppercase
    std::transform(method.begin(), method.end(), method.begin(), ::toupper);

    routes_.push_back({
        std::move(method),
        normalize_pattern(pattern),
        std::move(handler)
    });
}

RouteMatch Router::match(const std::string& method, const std::string& path) const {
    // Normalize incoming path (remove trailing slash except root)
    std::string clean_path = path;
    if (clean_path.size() > 1 && clean_path.back() == '/') clean_path.pop_back();

    for (const auto& route : routes_) {
        if (route.method != method && route.method != "*") continue;

        std::unordered_map<std::string, std::string> params;
        if (match_path(route.pattern, clean_path, params)) {
            return {true, route.handler, std::move(params)};
        }
    }
    return {false, nullptr, {}};
}

} // namespace osodio
