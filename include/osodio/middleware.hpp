#pragma once
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include "types.hpp"
#include "request.hpp"
#include "response.hpp"
#include "task.hpp"

namespace osodio {

// ─── logger() ─────────────────────────────────────────────────────────────────
//
// Logs every request with method, path, status code, and duration in ms.
// Because it co_awaits next(), it measures the FULL handler + middleware time
// (including async handlers).
//
//   app.use(osodio::logger());
//
// Optional: supply a custom output stream.
inline Middleware logger(std::ostream& out = std::cout) {
    return [&out](Request& req, Response& res, NextFn next) -> Task<void> {
        using Clock = std::chrono::steady_clock;
        auto t0 = Clock::now();

        co_await next();

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      Clock::now() - t0).count();
        out << req.method << ' ' << req.path
            << " → " << res.status_code()
            << " (" << ms << " ms)\n";
    };
}

// ─── CorsOptions ──────────────────────────────────────────────────────────────

struct CorsOptions {
    // Allowed origins.  Use {"*"} to allow any origin (default).
    // To allow specific origins: {"https://app.example.com", "http://localhost:5173"}
    std::vector<std::string> origins = {"*"};

    // Allowed HTTP methods (used in preflight response).
    std::vector<std::string> methods = {
        "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"
    };

    // Allowed request headers (used in preflight response).
    std::vector<std::string> headers = {
        "Content-Type", "Authorization", "X-Request-ID"
    };

    // Whether to allow cookies / credentials.
    bool credentials = false;

    // How long (seconds) the preflight may be cached by the browser.
    int max_age = 86400;
};

// ─── cors() ───────────────────────────────────────────────────────────────────
//
// Full CORS middleware: handles preflight OPTIONS automatically so it never
// reaches user handlers, and adds CORS headers to every real response.
//
//   app.use(osodio::cors());
//   app.use(osodio::cors({ .origins = {"https://app.example.com"} }));
//
inline Middleware cors(CorsOptions opts = {}) {
    return [opts = std::move(opts)](Request& req, Response& res, NextFn next) -> Task<void> {
        // Determine the effective Allow-Origin value
        std::string allow_origin;
        if (opts.origins.size() == 1 && opts.origins[0] == "*") {
            allow_origin = "*";
        } else {
            // Match the incoming Origin header against the allowlist
            auto origin_hdr = req.header("origin");
            if (origin_hdr) {
                for (const auto& o : opts.origins) {
                    if (o == *origin_hdr) { allow_origin = o; break; }
                }
            }
            // If no match, still add the header (browser will reject, that's correct)
            if (allow_origin.empty() && !opts.origins.empty())
                allow_origin = opts.origins[0];
        }

        res.header("Access-Control-Allow-Origin", allow_origin);
        if (allow_origin != "*")
            res.header("Vary", "Origin");
        if (opts.credentials)
            res.header("Access-Control-Allow-Credentials", "true");

        // ── Preflight (OPTIONS) ─────────────────────────────────────────────
        if (req.method == "OPTIONS") {
            // Build comma-separated lists
            std::string methods_str;
            for (size_t i = 0; i < opts.methods.size(); ++i) {
                if (i > 0) methods_str += ", ";
                methods_str += opts.methods[i];
            }
            std::string headers_str;
            for (size_t i = 0; i < opts.headers.size(); ++i) {
                if (i > 0) headers_str += ", ";
                headers_str += opts.headers[i];
            }
            res.header("Access-Control-Allow-Methods", methods_str);
            res.header("Access-Control-Allow-Headers", headers_str);
            res.header("Access-Control-Max-Age", std::to_string(opts.max_age));
            res.status(204).send("");
            co_return;   // do NOT call next() — preflight never reaches handlers
        }

        co_await next();
    };
}

} // namespace osodio
