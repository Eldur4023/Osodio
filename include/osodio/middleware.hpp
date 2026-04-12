#pragma once
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <zlib.h>
#include <brotli/encode.h>
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
// Compression middleware (Brotli preferred, gzip fallback).
// Runs after the handler; compresses the response body based on Accept-Encoding.
//
//   app.use(osodio::compress());
//   app.use(osodio::compress({.min_size=512, .level=9, .brotli_quality=5}));
//
// Priority: br > gzip (Brotli gives ~20% better ratio for text).
// Skips: already-compressed types (images, video, fonts, zip), bodies < min_size,
// clients that advertise neither encoding, 304/204 responses (no body).
//
struct CompressOptions {
    std::size_t min_size       = 1024;  // bytes — don't compress smaller responses
    int         level          = 6;     // gzip: zlib level 1–9  (1=fast, 9=best)
    int         brotli_quality = 6;     // brotli: 0–11 (0=fast, 11=best ratio)
};

inline Middleware compress(CompressOptions opts = {}) {
    return [opts](Request& req, Response& res, NextFn next) -> Task<void> {
        co_await next();

        auto ae = req.header("accept-encoding");
        if (!ae) co_return;

        bool want_br   = ae->find("br")   != std::string::npos;
        bool want_gzip = ae->find("gzip") != std::string::npos;
        if (!want_br && !want_gzip) co_return;

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

        // ── Brotli (preferred) ────────────────────────────────────────────────
        if (want_br) {
            std::size_t bound = BrotliEncoderMaxCompressedSize(body.size());
            std::string compressed(bound, '\0');
            std::size_t encoded_size = bound;

            BROTLI_BOOL ok = BrotliEncoderCompress(
                opts.brotli_quality,
                BROTLI_DEFAULT_WINDOW,
                BROTLI_MODE_TEXT,
                body.size(),
                reinterpret_cast<const uint8_t*>(body.data()),
                &encoded_size,
                reinterpret_cast<uint8_t*>(compressed.data())
            );
            if (ok == BROTLI_TRUE) {
                compressed.resize(encoded_size);
                res.header("Content-Encoding", "br")
                   .header("Vary", "Accept-Encoding")
                   .send(std::move(compressed));
                co_return;
            }
            // fall through to gzip on error
        }

        // ── gzip (fallback) ───────────────────────────────────────────────────
        if (!want_gzip) co_return;

        uLongf bound = compressBound(static_cast<uLong>(body.size())) + 18;
        std::string compressed(bound, '\0');

        z_stream zs{};
        zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(body.data()));
        zs.avail_in  = static_cast<uInt>(body.size());
        zs.next_out  = reinterpret_cast<Bytef*>(compressed.data());
        zs.avail_out = static_cast<uInt>(bound);

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

// ─── HelmetOptions ────────────────────────────────────────────────────────────

struct HelmetOptions {
    // Content-Security-Policy.  Set to "" to disable.
    std::string csp = "default-src 'self'";

    // HTTP Strict-Transport-Security.  Only meaningful behind HTTPS.
    // Set max_age to 0 to disable HSTS.
    int  hsts_max_age           = 15'552'000;   // 180 days
    bool hsts_include_subdomains = true;

    bool no_sniff   = true;   // X-Content-Type-Options: nosniff
    bool no_iframe  = true;   // X-Frame-Options: SAMEORIGIN
    bool xss_filter = false;  // X-XSS-Protection: 0  (disabled — modern browsers ignore it)
};

// ─── helmet() ─────────────────────────────────────────────────────────────────
//
// Adds common security headers to every response.  Zero-overhead: headers are
// built once at call time and reused from the closure.
//
//   app.use(osodio::helmet());
//   app.use(osodio::helmet({ .csp = "default-src 'self' https://cdn.example.com" }));
//
inline Middleware helmet(HelmetOptions opts = {}) {
    // Pre-build the header values once — no allocation per request.
    struct Headers { std::string key, value; };
    std::vector<Headers> hdrs;
    hdrs.reserve(6);

    if (!opts.csp.empty())
        hdrs.push_back({"Content-Security-Policy", opts.csp});
    if (opts.hsts_max_age > 0) {
        std::string hsts = "max-age=" + std::to_string(opts.hsts_max_age);
        if (opts.hsts_include_subdomains) hsts += "; includeSubDomains";
        hdrs.push_back({"Strict-Transport-Security", std::move(hsts)});
    }
    if (opts.no_sniff)
        hdrs.push_back({"X-Content-Type-Options", "nosniff"});
    if (opts.no_iframe)
        hdrs.push_back({"X-Frame-Options", "SAMEORIGIN"});
    if (!opts.xss_filter)
        hdrs.push_back({"X-XSS-Protection", "0"});
    hdrs.push_back({"Referrer-Policy", "strict-origin-when-cross-origin"});

    return [hdrs = std::move(hdrs)](Request& /*req*/, Response& res, NextFn next) -> Task<void> {
        co_await next();
        for (const auto& h : hdrs) res.header(h.key, h.value);
    };
}

// ─── RateLimitOptions ─────────────────────────────────────────────────────────

struct RateLimitOptions {
    // Maximum requests allowed per window per IP.
    int requests = 100;

    // Window length in seconds.
    int window_seconds = 60;

    // Custom key extractor.  Default: req.remote_ip.
    // Override to key by user ID, API key, etc.
    std::function<std::string(const Request&)> key_fn;

    // Message returned in the 429 response body.
    std::string message = "Too Many Requests";
};

// ─── rate_limit() ─────────────────────────────────────────────────────────────
//
// Fixed-window rate limiter per key (default: remote IP).
// State is per-middleware instance.  With SO_REUSEPORT multi-threading, each
// worker thread has independent limits — effective per-thread rate = total/threads.
//
//   app.use(osodio::rate_limit({ .requests = 60, .window_seconds = 60 }));
//   app.use(osodio::rate_limit({ .requests = 5, .window_seconds = 1 }));
//
inline Middleware rate_limit(RateLimitOptions opts = {}) {
    using Clock = std::chrono::steady_clock;

    struct Bucket {
        int               count = 0;
        Clock::time_point window_start;
    };

    std::function<std::string(const Request&)> key_fn =
        opts.key_fn ? opts.key_fn
                    : [](const Request& r) { return r.remote_ip; };

    // State is thread_local: each worker thread has independent counters.
    // With N threads, effective per-IP rate ≈ opts.requests × N.
    // This avoids cross-thread locking and is idiomatic for SO_REUSEPORT servers.
    return [opts, key_fn](Request& req, Response& res, NextFn next) -> Task<void> {
        using Seconds = std::chrono::seconds;
        thread_local std::unordered_map<std::string, Bucket> state;

        auto now = Clock::now();
        const std::string key = key_fn(req);

        auto& bucket = state[key];
        if (bucket.count == 0 ||
            std::chrono::duration_cast<Seconds>(now - bucket.window_start).count()
                >= opts.window_seconds) {
            bucket.count        = 0;
            bucket.window_start = now;
        }

        ++bucket.count;
        res.header("X-RateLimit-Limit",     std::to_string(opts.requests));
        res.header("X-RateLimit-Remaining", std::to_string(
            std::max(0, opts.requests - bucket.count)));

        if (bucket.count > opts.requests) {
            res.status(429).json({{"error", opts.message}});
            co_return;
        }

        co_await next();
    };
}

} // namespace osodio
