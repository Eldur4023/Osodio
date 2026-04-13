#pragma once

// JWT requires OpenSSL (HMAC-SHA256 / RSA-SHA256).
// When OSODIO_HAS_TLS is not defined the entire header is a no-op so that
// projects without OpenSSL can still include <osodio/osodio.hpp>.
#ifdef OSODIO_HAS_TLS

#include <string>
#include <string_view>
#include <stdexcept>
#include <chrono>
#include <optional>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <nlohmann/json.hpp>
#include "types.hpp"
#include "request.hpp"
#include "response.hpp"
#include "task.hpp"

namespace osodio {

// ── JwtError ──────────────────────────────────────────────────────────────────

struct JwtError : std::runtime_error {
    explicit JwtError(const char* msg) : std::runtime_error(msg) {}
};

// ── JwtOptions ────────────────────────────────────────────────────────────────

struct JwtOptions {
    // Expected algorithm.  Verified against the token header to prevent
    // algorithm-confusion attacks (e.g. RS256 key used as HS256 secret).
    std::string algorithm = "HS256";

    // Header from which the token is extracted.
    std::string header = "authorization";

    // If true, "Bearer <token>" is expected; the prefix is stripped.
    bool bearer = true;

    // Standard claim validation — skip by setting to nullopt.
    std::optional<std::string> issuer;    // "iss"
    std::optional<std::string> audience;  // "aud"
    bool check_exp = true;                // reject expired tokens
    bool check_nbf = true;                // reject not-yet-valid tokens
};

// ── Low-level JWT helpers ─────────────────────────────────────────────────────

namespace detail {

inline std::string base64url_encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = uint32_t(data[i]) << 16
                   | (i+1 < len ? uint32_t(data[i+1]) << 8  : 0)
                   | (i+2 < len ? uint32_t(data[i+2])       : 0);
        out += kTable[(b >> 18) & 63];
        out += kTable[(b >> 12) & 63];
        out += (i+1 < len) ? kTable[(b >> 6) & 63] : '\0';
        out += (i+2 < len) ? kTable[ b        & 63] : '\0';
    }
    // Remove null-terminators used as padding placeholders
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

inline std::string base64url_encode(const std::string& s) {
    return base64url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Returns decoded bytes as string; throws JwtError on invalid input.
inline std::string base64url_decode(std::string_view in) {
    static const int8_t kDec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0–15
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 16–31
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1, // 32–47  (-=62)
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1, // 48–63  (0-9)
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 64–79  (A-O)
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63, // 80–95  (P-Z, _=63)
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 96–111 (a-o)
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // 112–127(p-z)
    };
    std::string out;
    out.reserve((in.size() * 3) / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int8_t v = (static_cast<uint8_t>(c) < 128) ? kDec[static_cast<uint8_t>(c)] : -1;
        if (v < 0) throw JwtError("invalid base64url character");
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xFF);
        }
    }
    return out;
}

inline std::string hmac_sha256(const std::string& key, const std::string& msg) {
    uint8_t digest[32];
    unsigned int len = 32;
    HMAC(EVP_sha256(),
         key.data(),  static_cast<int>(key.size()),
         reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
         digest, &len);
    return std::string(reinterpret_cast<char*>(digest), len);
}

// Verify an RSA-SHA256 signature over `message` using PEM public key.
inline bool rsa_sha256_verify(const std::string& public_key_pem,
                               const std::string& message,
                               const std::string& sig) {
    BIO* bio = BIO_new_mem_buf(public_key_pem.data(),
                                static_cast<int>(public_key_pem.size()));
    if (!bio) return false;
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx) {
        if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            EVP_DigestVerifyUpdate(ctx,
                reinterpret_cast<const uint8_t*>(message.data()),
                message.size()) == 1) {
            ok = (EVP_DigestVerifyFinal(ctx,
                reinterpret_cast<const uint8_t*>(sig.data()),
                sig.size()) == 1);
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

// Sign with RSA-SHA256 using PEM private key.
inline std::string rsa_sha256_sign(const std::string& private_key_pem,
                                    const std::string& message) {
    BIO* bio = BIO_new_mem_buf(private_key_pem.data(),
                                static_cast<int>(private_key_pem.size()));
    if (!bio) throw JwtError("RS256: cannot load private key");
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) throw JwtError("RS256: invalid private key PEM");

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pkey); throw JwtError("RS256: EVP_MD_CTX_new"); }

    size_t siglen = 0;
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1 ||
        EVP_DigestSignUpdate(ctx,
            reinterpret_cast<const uint8_t*>(message.data()), message.size()) != 1 ||
        EVP_DigestSignFinal(ctx, nullptr, &siglen) != 1) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey);
        throw JwtError("RS256: sign failed");
    }
    std::string sig(siglen, '\0');
    EVP_DigestSignFinal(ctx, reinterpret_cast<uint8_t*>(sig.data()), &siglen);
    sig.resize(siglen);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return sig;
}

} // namespace detail

// ── jwt::sign / verify / decode ───────────────────────────────────────────────

namespace jwt {

// Create a signed JWT.
//
//   auto token = jwt::sign({{"sub","1234"},{"name","Alice"},{"exp", exp_ts}},
//                           my_secret);
//
// alg: "HS256" (default) or "RS256" (key = PEM private key string)
inline std::string sign(const nlohmann::json& payload,
                         const std::string& key,
                         const std::string& alg = "HS256") {
    nlohmann::json hdr = {{"alg", alg}, {"typ", "JWT"}};
    std::string h = detail::base64url_encode(hdr.dump());
    std::string p = detail::base64url_encode(payload.dump());
    std::string signing_input = h + '.' + p;
    std::string sig;
    if (alg == "HS256") {
        sig = detail::hmac_sha256(key, signing_input);
    } else if (alg == "RS256") {
        sig = detail::rsa_sha256_sign(key, signing_input);
    } else {
        throw JwtError("unsupported algorithm");
    }
    return signing_input + '.' + detail::base64url_encode(sig);
}

// Decode and verify a JWT.  Throws JwtError on any failure.
// Returns the payload claims as a JSON object.
inline nlohmann::json verify(const std::string& token,
                               const std::string& key,
                               const JwtOptions& opts = {}) {
    // Split into 3 parts
    auto d1 = token.find('.');
    if (d1 == std::string::npos) throw JwtError("malformed token");
    auto d2 = token.find('.', d1 + 1);
    if (d2 == std::string::npos) throw JwtError("malformed token");

    std::string hdr_b64  = token.substr(0, d1);
    std::string pay_b64  = token.substr(d1 + 1, d2 - d1 - 1);
    std::string sig_b64  = token.substr(d2 + 1);
    std::string signing_input = hdr_b64 + '.' + pay_b64;

    // Decode and parse header
    nlohmann::json hdr;
    try { hdr = nlohmann::json::parse(detail::base64url_decode(hdr_b64)); }
    catch (...) { throw JwtError("invalid header"); }

    // Algorithm check — prevent confusion attacks
    std::string alg = hdr.value("alg", "");
    if (alg != opts.algorithm)
        throw JwtError("algorithm mismatch");

    // Verify signature
    std::string sig = detail::base64url_decode(sig_b64);
    if (opts.algorithm == "HS256") {
        std::string expected = detail::hmac_sha256(key, signing_input);
        if (sig.size() != expected.size() ||
            CRYPTO_memcmp(sig.data(), expected.data(), sig.size()) != 0)
            throw JwtError("invalid signature");
    } else if (opts.algorithm == "RS256") {
        if (!detail::rsa_sha256_verify(key, signing_input, sig))
            throw JwtError("invalid signature");
    } else {
        throw JwtError("unsupported algorithm");
    }

    // Decode and parse payload
    nlohmann::json claims;
    try { claims = nlohmann::json::parse(detail::base64url_decode(pay_b64)); }
    catch (...) { throw JwtError("invalid payload"); }

    // Standard claim validation
    using Clock = std::chrono::system_clock;
    auto now = Clock::now().time_since_epoch() / std::chrono::seconds(1);

    if (opts.check_exp && claims.contains("exp")) {
        if (claims["exp"].get<int64_t>() < now)
            throw JwtError("token expired");
    }
    if (opts.check_nbf && claims.contains("nbf")) {
        if (claims["nbf"].get<int64_t>() > now)
            throw JwtError("token not yet valid");
    }
    if (opts.issuer && claims.value("iss", "") != *opts.issuer)
        throw JwtError("issuer mismatch");
    if (opts.audience) {
        // "aud" may be a string or an array
        bool found = false;
        if (claims.contains("aud")) {
            auto& a = claims["aud"];
            if (a.is_string())      found = (a.get<std::string>() == *opts.audience);
            else if (a.is_array()) {
                for (auto& v : a)
                    if (v.is_string() && v.get<std::string>() == *opts.audience)
                        { found = true; break; }
            }
        }
        if (!found) throw JwtError("audience mismatch");
    }

    return claims;
}

// Decode a JWT without verifying the signature (for inspection only).
inline nlohmann::json decode(const std::string& token) {
    auto d1 = token.find('.');
    if (d1 == std::string::npos) throw JwtError("malformed token");
    auto d2 = token.find('.', d1 + 1);
    if (d2 == std::string::npos) throw JwtError("malformed token");
    try {
        return nlohmann::json::parse(
            detail::base64url_decode(token.substr(d1 + 1, d2 - d1 - 1)));
    } catch (...) { throw JwtError("invalid payload"); }
}

// Helper: build an exp timestamp N seconds from now.
inline int64_t expires_in(int seconds) {
    using Clock = std::chrono::system_clock;
    return Clock::now().time_since_epoch() / std::chrono::seconds(1) + seconds;
}

} // namespace jwt

// ── jwt_auth() middleware ─────────────────────────────────────────────────────
//
// Validates a JWT on every request.  On success, claims are available via
// req.jwt_claims.  On failure, responds 401 and short-circuits the chain.
//
//   // HS256 (shared secret)
//   app.use(osodio::jwt_auth("my-secret"));
//
//   // RS256 (RSA public key)
//   app.use(osodio::jwt_auth(public_key_pem, {.algorithm = "RS256"}));
//
//   // Skip auth on public routes
//   app.use(osodio::jwt_auth("secret", {.skip = [](auto& req){
//       return req.path == "/login";
//   }}));
//
struct JwtAuthOptions : JwtOptions {
    // Optional predicate — returning true bypasses JWT validation for that request.
    std::function<bool(const Request&)> skip;
};

inline Middleware jwt_auth(std::string key, JwtAuthOptions opts = {}) {
    return [key = std::move(key), opts = std::move(opts)]
           (Request& req, Response& res, NextFn next) -> Task<void> {

        // Skip check (e.g. login / health routes)
        if (opts.skip && opts.skip(req)) {
            co_await next();
            co_return;
        }

        // Extract token from header
        auto hdr_val = req.header(opts.header);
        if (!hdr_val) {
            res.status(401).json({{"error", "missing authorization header"}});
            co_return;
        }

        std::string token = *hdr_val;
        if (opts.bearer) {
            // Expect "Bearer <token>" (case-insensitive prefix)
            std::string lower = token;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.rfind("bearer ", 0) != 0) {
                res.status(401).json({{"error", "expected Bearer token"}});
                co_return;
            }
            token = token.substr(7);
        }

        // Verify
        try {
            req.jwt_claims = jwt::verify(token, key, opts);
        } catch (const JwtError& e) {
            res.status(401).json({{"error", e.what()}});
            co_return;
        }

        co_await next();
    };
}

// RS256 convenience overload — pass the PEM public key directly.
inline Middleware jwt_auth_rsa(std::string public_key_pem, JwtAuthOptions opts = {}) {
    opts.algorithm = "RS256";
    return jwt_auth(std::move(public_key_pem), std::move(opts));
}

} // namespace osodio

#endif // OSODIO_HAS_TLS
