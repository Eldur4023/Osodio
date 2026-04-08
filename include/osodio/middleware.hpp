#pragma once
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <zlib.h>
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

// ─── compress() ───────────────────────────────────────────────────────────────
//
// gzip compression middleware.  Runs after the handler; compresses the response
// body if the client signals Accept-Encoding: gzip and the body is large enough.
//
//   app.use(osodio::compress());                // default: min_size=1024, level=6
//   app.use(osodio::compress({.min_size=512, .level=9}));
//
// Skips: already-compressed types (images, video, fonts, zip), bodies < min_size,
// clients that don't accept gzip, 304/204 responses (no body).
//
struct CompressOptions {
    std::size_t min_size = 1024;   // bytes — don't compress smaller responses
    int         level    = 6;      // zlib level 1-9  (1=fast, 9=best ratio)
};

inline Middleware compress(CompressOptions opts = {}) {
    return [opts](Request& req, Response& res, NextFn next) -> Task<void> {
        co_await next();

        // Only compress if client advertises gzip
        auto ae = req.header("accept-encoding");
        if (!ae) co_return;
        if (ae->find("gzip") == std::string::npos) co_return;

        // Skip no-body statuses
        int sc = res.status_code();
        if (sc == 204 || sc == 304) co_return;

        // Skip small responses
        const std::string& body = res.body();
        if (body.size() < opts.min_size) co_return;

        // Skip already-compressed content types
        static const std::vector<std::string> skip_prefixes = {
            "image/", "video/", "audio/",
            "font/woff", "font/woff2",
            "application/zip", "application/gzip",
            "application/octet-stream"
        };
        auto ct_hdr = res.content_type();
        if (!ct_hdr.empty()) {
            for (const auto& pfx : skip_prefixes) {
                if (ct_hdr.rfind(pfx, 0) == 0) co_return;
            }
        }

        // ── gzip compress with zlib ───────────────────────────────────────────
        uLongf bound = compressBound(static_cast<uLong>(body.size())) + 18; // gzip header
        std::string compressed(bound, '\0');

        z_stream zs{};
        zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(body.data()));
        zs.avail_in  = static_cast<uInt>(body.size());
        zs.next_out  = reinterpret_cast<Bytef*>(compressed.data());
        zs.avail_out = static_cast<uInt>(bound);

        // windowBits = 15 + 16 → gzip format (not raw deflate)
        if (deflateInit2(&zs, opts.level, Z_DEFLATED,
                         15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) co_return;

        int ret = deflate(&zs, Z_FINISH);
        deflateEnd(&zs);
        if (ret != Z_STREAM_END) co_return;

        compressed.resize(zs.total_out);

        res.header("Content-Encoding", "gzip")
           .header("Vary", "Accept-Encoding")
           .send(std::move(compressed));
    };
}

} // namespace osodio
