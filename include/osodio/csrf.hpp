#pragma once

// CSRF protection middleware.  Requires OpenSSL for RAND_bytes + constant-time
// comparison; the header is a no-op when OSODIO_HAS_TLS is undefined.
#ifdef OSODIO_HAS_TLS

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <fstream>
#include <cstdint>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include "types.hpp"
#include "request.hpp"
#include "response.hpp"
#include "cookies.hpp"
#include "task.hpp"

namespace osodio {

// ─── CsrfOptions ─────────────────────────────────────────────────────────────
//
// Double-submit cookie pattern (stateless, no server-side store):
//
//   1. Server sets a random token in a `csrf_token` cookie (HttpOnly=false so
//      JavaScript can read it and echo it in a header — standard pattern).
//   2. On state-changing requests, browser-side JS reads the cookie and sends
//      the same value in `X-CSRF-Token`.  Server compares the two with
//      CRYPTO_memcmp.
//   3. A cross-origin attacker can force the browser to send the cookie but
//      cannot read it (same-origin policy applies to cookie reads via JS) —
//      so they cannot also forge the header.  Mismatch → 403.
//
// Activation is conditional, mirroring how Django/Spring handle it:
//   • Only enforced on POST/PUT/PATCH/DELETE.
//   • Skipped when an `Authorization:` header is present (token-auth — no
//     ambient session, no CSRF surface).
//   • Skipped when the request has no `Cookie:` header (no browser session).
//
// This makes csrf() safe to apply globally: API clients with bearer tokens
// are unaffected; only cookie-authenticated browser flows are protected.
//
struct CsrfOptions {
    // Cookie name carrying the token.
    std::string cookie_name = "csrf_token";

    // Header name the client must echo back.
    std::string header_name = "x-csrf-token";

    // Cookie attributes.  HttpOnly defaults to false so client-side JS can
    // read the token (required by the double-submit pattern).
    CookieOptions cookie_opts = {
        .path      = "/",
        .secure    = true,
        .http_only = false,
        .same_site = SameSite::Lax,
    };

    // Methods that require validation.  Safe methods (GET, HEAD, OPTIONS,
    // TRACE) are never validated.
    std::vector<std::string> protected_methods = {"POST", "PUT", "PATCH", "DELETE"};

    // Optional bypass predicate (return true to skip the check).
    std::function<bool(const Request&)> skip;
};

namespace detail {

// 32 random bytes → 43-char URL-safe base64 string (no padding).
inline std::string generate_csrf_token() {
    uint8_t raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        // Cryptographic RNG failed — fall back to /dev/urandom directly.
        std::ifstream f("/dev/urandom", std::ios::binary);
        f.read(reinterpret_cast<char*>(raw), sizeof(raw));
        if (!f) return {};  // exhausted — caller will treat as missing token
    }
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(43);
    for (size_t i = 0; i + 2 < sizeof(raw); i += 3) {
        uint32_t b = (uint32_t(raw[i]) << 16)
                   | (uint32_t(raw[i+1]) << 8)
                   |  uint32_t(raw[i+2]);
        out += kTable[(b >> 18) & 63];
        out += kTable[(b >> 12) & 63];
        out += kTable[(b >>  6) & 63];
        out += kTable[ b        & 63];
    }
    // 32 bytes → 10 full triplets + 2 leftover bytes (16 bits → 3 base64 chars)
    uint32_t b = (uint32_t(raw[30]) << 16) | (uint32_t(raw[31]) << 8);
    out += kTable[(b >> 18) & 63];
    out += kTable[(b >> 12) & 63];
    out += kTable[(b >>  6) & 63];
    return out;
}

inline bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

inline bool is_safe_method(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "OPTIONS" || m == "TRACE";
}

inline bool is_protected_method(const std::string& m,
                                 const std::vector<std::string>& protected_) {
    for (const auto& p : protected_) if (p == m) return true;
    return false;
}

} // namespace detail

// ─── csrf() ──────────────────────────────────────────────────────────────────
//
// Double-submit CSRF middleware.  Safe to apply globally:
//
//   app.use(osodio::csrf());
//
// Configure the cookie attributes for production (Secure already on by default):
//
//   app.use(osodio::csrf({
//       .cookie_opts = {.secure = true, .same_site = SameSite::Strict},
//   }));
//
inline Middleware csrf(CsrfOptions opts = {}) {
    return [opts = std::move(opts)]
           (Request& req, Response& res, NextFn next) -> Task<void> {

        // Bypass predicate (e.g. webhook endpoints)
        if (opts.skip && opts.skip(req)) {
            co_await next();
            co_return;
        }

        // ── 1. Ensure a token cookie exists ──────────────────────────────────
        // If absent, generate one for this response.  This means the first
        // safe request after page load primes the cookie; the JS code can
        // then read it and echo it on subsequent mutating calls.
        auto existing = req.cookie(opts.cookie_name);
        std::string token = existing.value_or("");
        if (token.empty()) {
            token = detail::generate_csrf_token();
            if (!token.empty())
                res.cookie(opts.cookie_name, token, opts.cookie_opts);
        }

        // ── 2. Validate on mutating requests ─────────────────────────────────
        if (!detail::is_safe_method(req.method) &&
            detail::is_protected_method(req.method, opts.protected_methods))
        {
            // Skip if this is a token-auth (no ambient cookie session) flow.
            // The presence of Authorization implies the client is sending a
            // bearer/explicit token and isn't relying on cookies for auth.
            if (req.header("authorization")) {
                co_await next();
                co_return;
            }
            // Skip if the request carries no cookies at all (cannot be a
            // cookie-authenticated browser session).
            if (!req.header("cookie")) {
                co_await next();
                co_return;
            }

            // Validate: header must match cookie, constant-time.
            auto hdr = req.header(opts.header_name);
            if (!existing || !hdr ||
                !detail::constant_time_equal(*existing, *hdr))
            {
                res.status(403).json({{"error", "CSRF token missing or invalid"}});
                co_return;
            }
        }

        co_await next();
    };
}

} // namespace osodio

#endif // OSODIO_HAS_TLS
