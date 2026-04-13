#include "../include/osodio/app.hpp"
#include "../include/osodio/metrics.hpp"
#ifdef OSODIO_HAS_TLS
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#endif
#include "../include/osodio/request.hpp"
#include "../include/osodio/response.hpp"
#include "../include/osodio/task.hpp"

#include <osodio/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <mutex>
#include <functional>
#include <sstream>
#include <iomanip>

namespace osodio {

namespace {

#ifdef OSODIO_HAS_TLS
// ── ALPN protocol selection ───────────────────────────────────────────────────
// Prefer "h2" (HTTP/2); accept "http/1.1" as fallback.
// Called during TLS handshake when the client presents its ALPN list.
static int alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen, void*)
{
    const unsigned char* p   = in;
    const unsigned char* end = in + inlen;
    const unsigned char* h11 = nullptr;
    unsigned char        h11_len = 0;

    while (p < end) {
        unsigned char len = *p++;
        if (p + len > end) break;
        if (len == 2 && memcmp(p, "h2",       2) == 0) { *out = p; *outlen = len; return SSL_TLSEXT_ERR_OK; }
        if (len == 8 && memcmp(p, "http/1.1", 8) == 0) { h11 = p; h11_len = len; }
        p += len;
    }
    if (h11) { *out = h11; *outlen = h11_len; return SSL_TLSEXT_ERR_OK; }
    return SSL_TLSEXT_ERR_NOACK;
}
#endif // OSODIO_HAS_TLS

static const char* mime_for_ext(const std::string& ext) {
    if (ext == ".html" || ext == ".htm")  return "text/html; charset=utf-8";
    if (ext == ".css")   return "text/css; charset=utf-8";
    if (ext == ".js")    return "application/javascript; charset=utf-8";
    if (ext == ".json")  return "application/json; charset=utf-8";
    if (ext == ".svg")   return "image/svg+xml";
    if (ext == ".png")   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")   return "image/gif";
    if (ext == ".webp")  return "image/webp";
    if (ext == ".ico")   return "image/x-icon";
    if (ext == ".woff")  return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf")   return "font/ttf";
    if (ext == ".pdf")   return "application/pdf";
    if (ext == ".xml")   return "application/xml";
    if (ext == ".txt")   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// Weak ETag from mtime + size: "mtime-size" hex-encoded.
static std::string make_etag(const std::filesystem::file_time_type& mtime,
                              std::uintmax_t size) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  mtime.time_since_epoch()).count();
    std::ostringstream ss;
    ss << '"' << std::hex << ns << '-' << size << '"';
    return ss.str();
}

// Returns true and fills res if a static mount covers this path.
// Sets ETag, Cache-Control, and honours If-None-Match for 304 responses.
static bool try_serve_static(
    const std::vector<App::StaticMount>& mounts,
    const Request& req,
    Response& res)
{
    namespace fs = std::filesystem;
    for (const auto& m : mounts) {
        if (req.path.rfind(m.prefix, 0) != 0) continue;

        std::string rel = req.path.substr(m.prefix.size());
        if (rel.empty() || rel.front() != '/') rel = '/' + rel;

        fs::path file = fs::path(m.root) / rel.substr(1);

        auto canonical_root = fs::weakly_canonical(m.root);
        auto canonical_file = fs::weakly_canonical(file);
        auto [ri, fi] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                      canonical_file.begin());
        if (ri != canonical_root.end()) {
            res.status(403).json({{"error", "Forbidden"}});
            return true;
        }

        std::error_code ec;
        auto status = fs::status(canonical_file, ec);
        if (ec || !fs::is_regular_file(status)) {
            // SPA fallback: serve index.html for unknown paths so client-side
            // routers (React Router, Vue Router, etc.) can handle the URL.
            if (m.spa) {
                canonical_file = fs::weakly_canonical(fs::path(m.root) / "index.html");
                auto spa_status = fs::status(canonical_file, ec);
                if (ec || !fs::is_regular_file(spa_status)) {
                    res.status(404).json({{"error", "Not Found"}});
                    return true;
                }
                // Continue below with canonical_file = index.html
            } else {
                res.status(404).json({{"error", "Not Found"}});
                return true;
            }
        }

        // ── ETag ──────────────────────────────────────────────────────────────
        auto mtime    = fs::last_write_time(canonical_file, ec);
        auto filesize = fs::file_size(canonical_file, ec);
        std::string etag = make_etag(mtime, filesize);

        // ── Cache-Control ─────────────────────────────────────────────────────
        // Hashed filenames (e.g. app.abc123.js) → immutable for 1 year.
        // Everything else → must-revalidate with short max-age.
        const std::string& ext = canonical_file.extension().string();
        const std::string  stem = canonical_file.stem().string();
        bool looks_hashed = stem.size() > 8 &&
                            stem.find('.') != std::string::npos; // app.abc123
        const char* cache_ctrl = looks_hashed
            ? "public, max-age=31536000, immutable"
            : "public, max-age=3600, must-revalidate";

        res.header("ETag",          etag);
        res.header("Cache-Control", cache_ctrl);

        // ── 304 Not Modified ──────────────────────────────────────────────────
        auto inm = req.header("if-none-match");
        if (inm && *inm == etag) {
            res.status(304).send("");
            return true;
        }

        // ── Serve via sendfile(2) — zero-copy ─────────────────────────────────
        const char* mime = mime_for_ext(ext);
        res.header("Content-Type", mime).send_file(canonical_file);
        return true;
    }
    return false;
}

// Graceful shutdown state — written once before spawning threads, called from
// the signal handler.  The signal handler itself must remain async-signal-safe;
// the heavy work is dispatched onto the event loop thread via post().
static std::function<void()> g_initiate_drain;
static std::atomic<bool>     g_shutting_down{false};

static void signal_handler(int) {
    if (g_shutting_down.exchange(true)) {
        // Second signal: user is impatient — force exit immediately.
        std::cout << "\nForced exit.\n";
        std::_Exit(1);
    }
    std::cout << "\nShutting down gracefully... (CTRL+C again to force)\n";
    if (g_initiate_drain) g_initiate_drain();
}

} // anonymous namespace

// ── App::run ──────────────────────────────────────────────────────────────────

// ── App::prepare ─────────────────────────────────────────────────────────────
// Registers docs routes once (idempotent). Safe to call multiple times.

void App::prepare() {
    if (prepared_) return;
    prepared_ = true;
    if (openapi_enabled_) {
        std::string spec = build_openapi_doc(api_title_, api_version_, openapi_routes_).dump(2);
        std::string spec_path = openapi_spec_path_;
        std::string ui_path   = openapi_ui_path_;
        router_.add("GET", spec_path, [spec](Request&, Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(spec);
        });
        router_.add("GET", ui_path, [spec_path](Request&, Response& res) {
            res.html(swagger_ui_html(spec_path));
        });
    }
}

// ── App::handle_request ───────────────────────────────────────────────────────
// Full middleware + router pipeline as a coroutine.
// Used by run() (via the DispatchFn) and by TestClient for in-process testing.

Task<void> App::handle_request(Request& req, Response& res) {
    res.set_templates_dir(templates_dir_);
    req.container = &container_;

    // Static file mounts bypass the middleware chain.
    if (req.method == "GET" || req.method == "HEAD") {
        if (try_serve_static(static_mounts_, req, res)) co_return;
    }

    // ── Async middleware chain ─────────────────────────────────────────────
    // call_next is a local in THIS coroutine frame (heap-allocated).
    // Inner lambdas capture &call_next (reference into the frame), which
    // remains valid as long as this coroutine is suspended or running.
    std::function<Task<void>(size_t)> call_next;
    call_next = [this, &req, &res, &call_next](size_t i) -> Task<void> {
        if (i < middlewares_.size()) {
            co_await middlewares_[i](req, res,
                [&call_next, i]() -> Task<void> {
                    return call_next(i + 1);
                });
        } else {
            auto match = router_.match(req.method, req.path);
            if (match.found) {
                req.params = std::move(match.params);
                co_await match.handler(req, res);
            } else {
                res.status(404).json({{"error", "Not Found"}, {"path", req.path}});
            }
        }
    };
    co_await call_next(0);

    // Error handlers run after the full chain, while we still own req/res.
    if (res.status_code() >= 400) {
        int code = res.status_code();
        auto it = error_handlers_.find(code);
        if (it != error_handlers_.end()) {
            it->second(code, req, res);
        } else if (catchall_error_handler_) {
            catchall_error_handler_(code, req, res);
        }
    }
}

// ── App::run ──────────────────────────────────────────────────────────────────

void App::run(const std::string& host, uint16_t port) {
    std::signal(SIGPIPE, SIG_IGN);

    prepare();  // register docs routes if enable_docs() was called

    // ── Build the async dispatch function ─────────────────────────────────────
    // Returns handle_request() directly — no extra coroutine frame.
    DispatchFn dispatch = [this](Request& req, Response& res) {
        return handle_request(req, res);
    };

    // ── Multi-core: one event loop per hardware thread ────────────────────────
    unsigned num_threads = std::max(1u, std::thread::hardware_concurrency());

    // ── TLS context ───────────────────────────────────────────────────────────
    // Created once here; SSL_CTX is thread-safe for SSL_new() across threads.
    // Passed to each TcpServer → HttpConnection as a raw pointer (lifetime is
    // the entire duration of run(), guarded by ssl_ctx_guard below).
    SSL_CTX* ssl_ctx = nullptr;
#ifdef OSODIO_HAS_TLS
    std::shared_ptr<SSL_CTX> ssl_ctx_guard; // ensures SSL_CTX_free on exit

    if (!ssl_cert_.empty()) {
        ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!ssl_ctx) throw std::runtime_error("SSL_CTX_new failed");

        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
        SSL_CTX_set_mode(ssl_ctx,
            SSL_MODE_ENABLE_PARTIAL_WRITE |
            SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

        if (SSL_CTX_use_certificate_chain_file(ssl_ctx, ssl_cert_.c_str()) != 1)
            throw std::runtime_error("Failed to load certificate: " + ssl_cert_);

        if (SSL_CTX_use_PrivateKey_file(ssl_ctx, ssl_key_.c_str(), SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("Failed to load private key: " + ssl_key_);

        if (SSL_CTX_check_private_key(ssl_ctx) != 1)
            throw std::runtime_error("Certificate and private key do not match");

        // Advertise h2 and http/1.1 via ALPN so browsers can upgrade to HTTP/2.
        SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_cb, nullptr);

        ssl_ctx_guard = std::shared_ptr<SSL_CTX>(ssl_ctx, SSL_CTX_free);
    }
#endif // OSODIO_HAS_TLS

    // Shared connection counter — enforces max_connections_ across all threads.
    auto shared_conn_count = std::make_shared<std::atomic<int>>(0);
    Metrics::instance().active_connections_ = shared_conn_count.get();

    std::vector<core::EventLoop*>  all_loops;
    std::vector<core::TcpServer*>  all_servers;
    std::mutex                     all_mutex;

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    // On first SIGINT/SIGTERM:
    //   1. Stop accepting new connections on all workers.
    //   2. Post a drain-checker task to the main event loop that polls every
    //      100 ms.  When conn_count reaches 0 (or 30 s elapse), all loops are
    //      stopped cleanly.
    // On second signal: std::_Exit(1) — see signal_handler above.
    g_shutting_down = false;
    core::EventLoop main_loop;   // declared before g_initiate_drain so it can be captured
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_loops.push_back(&main_loop);
    }

    g_initiate_drain = [&main_loop, shared_conn_count, &all_loops, &all_servers, &all_mutex]() {
        // Stop all acceptors (runs on signal-handler thread; post() is used
        // for the heavy work so it executes on the event loop thread).
        {
            std::lock_guard<std::mutex> lk(all_mutex);
            for (auto* s : all_servers) s->stop_accepting();
        }

        // Dispatch the drain poll to the main loop thread.
        main_loop.post([&main_loop, shared_conn_count, &all_loops, &all_mutex]() {
            using Clock = std::chrono::steady_clock;
            auto deadline = Clock::now() + std::chrono::seconds(30);

            auto fn = std::make_shared<std::function<void()>>();
            *fn = [fn, &main_loop, shared_conn_count, deadline,
                   &all_loops, &all_mutex]() mutable {
                bool timed_out = Clock::now() >= deadline;
                int  remaining = shared_conn_count->load(std::memory_order_acquire);

                if (remaining == 0 || timed_out) {
                    if (timed_out && remaining > 0)
                        std::cout << "Grace period expired — " << remaining
                                  << " connection(s) dropped.\n";
                    else
                        std::cout << "All connections drained.\n";
                    std::lock_guard<std::mutex> lk(all_mutex);
                    for (auto* l : all_loops) l->stop();
                    return;
                }
                main_loop.schedule_timer(100, *fn);
            };
            (*fn)();  // first check immediately
        });
    };

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Worker threads (cores 1..N-1) ─────────────────────────────────────────
    // Each thread runs its own EventLoop + TcpServer.  SO_REUSEPORT lets the
    // kernel distribute incoming connections evenly across all workers.
    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (unsigned i = 1; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            core::EventLoop loop;
            core::TcpServer server(host, port, loop, dispatch,
                                   max_connections_, shared_conn_count, ssl_ctx);
            {
                std::lock_guard<std::mutex> lk(all_mutex);
                all_loops.push_back(&loop);
                all_servers.push_back(&server);
            }
            loop.run();
            {
                // Remove from tracking after the loop exits so stop_accepting()
                // is never called on a destroyed server.
                std::lock_guard<std::mutex> lk(all_mutex);
                all_loops.erase(std::remove(all_loops.begin(), all_loops.end(), &loop), all_loops.end());
                all_servers.erase(std::remove(all_servers.begin(), all_servers.end(), &server), all_servers.end());
            }
        });
    }

    // ── Main thread (core 0) ──────────────────────────────────────────────────
    core::TcpServer main_server(host, port, main_loop, dispatch,
                                max_connections_, shared_conn_count, ssl_ctx);
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_servers.push_back(&main_server);
    }

    const char* scheme = ssl_ctx ? "https" : "http";
    std::cout << " * Osodio running on " << scheme << "://" << host << ":" << port
              << "  (threads=" << num_threads << ", press CTRL+C to quit)\n";

    main_loop.run();

    for (auto& t : threads) t.join();

    Metrics::instance().active_connections_ = nullptr;
    g_initiate_drain = nullptr;
    g_shutting_down  = false;
}

} // namespace osodio
