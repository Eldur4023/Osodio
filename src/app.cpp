#include "../include/osodio/app.hpp"
#include "../include/osodio/request.hpp"
#include "../include/osodio/response.hpp"
#include "../include/osodio/task.hpp"

#include <osodio/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <fstream>
#include <filesystem>
#include <thread>
#include <vector>
#include <mutex>
#include <functional>

namespace osodio {

namespace {

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

// Returns true and fills res if a static mount covers this path.
static bool try_serve_static(
    const std::vector<App::StaticMount>& mounts,
    const std::string& req_path,
    Response& res)
{
    namespace fs = std::filesystem;
    for (const auto& m : mounts) {
        if (req_path.rfind(m.prefix, 0) != 0) continue;

        std::string rel = req_path.substr(m.prefix.size());
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

        if (!fs::exists(canonical_file) || !fs::is_regular_file(canonical_file)) {
            res.status(404).json({{"error", "Not Found"}});
            return true;
        }

        std::ifstream f(canonical_file, std::ios::binary);
        if (!f) { res.status(500).json({{"error", "Could not read file"}}); return true; }

        std::string body{std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>()};
        const char* mime = mime_for_ext(canonical_file.extension().string());
        res.header("Content-Type", mime).send(std::move(body));
        return true;
    }
    return false;
}

// Global stop callback — set before spawning threads, read only after.
static std::function<void()> g_stop_all;

static void signal_handler(int) {
    std::cout << "\nShutting down...\n";
    if (g_stop_all) g_stop_all();
}

} // anonymous namespace

// ── App::run ──────────────────────────────────────────────────────────────────

void App::run(const std::string& host, uint16_t port) {
    std::signal(SIGPIPE, SIG_IGN);

    // ── Built-in: /openapi.json and /docs ─────────────────────────────────────
    // Build the spec once, capture by value into the handler lambda.
    {
        std::string spec = build_openapi_doc(api_title_, api_version_, openapi_routes_).dump(2);
        router_.add("GET", "/openapi.json", [spec](Request&, Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(spec);
        });
        router_.add("GET", "/docs", [](Request&, Response& res) {
            res.html(swagger_ui_html());
        });
    }

    // ── Build the async dispatch function ─────────────────────────────────────
    // Captured by value into the DispatchFn so each thread gets its own copy.
    // All captures are read-only after run() starts, so thread-safe.
    DispatchFn dispatch = [this](Request& req, Response& res) -> Task<void> {
        res.set_templates_dir(templates_dir_);
        req.container = &container_;

        // Static file mounts bypass the middleware chain.
        if (req.method == "GET" || req.method == "HEAD") {
            if (try_serve_static(static_mounts_, req.path, res)) co_return;
        }

        // ── Async middleware chain ─────────────────────────────────────────
        // call_next is a local in THIS coroutine frame (heap-allocated).
        // Inner lambdas capture &call_next (reference into the frame), which
        // remains valid as long as this coroutine is suspended or running.
        std::function<Task<void>(size_t)> call_next;
        call_next = [this, &req, &res, &call_next](size_t i) -> Task<void> {
            if (i < middlewares_.size()) {
                // Pass next() as a NextFn that returns call_next(i+1)
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
    };

    // ── Multi-core: one event loop per hardware thread ────────────────────────
    unsigned num_threads = 1; // Forced for debugging

    std::vector<core::EventLoop*> all_loops;
    std::mutex loops_mutex;

    // Stop callback: called from SIGINT/SIGTERM, stops every loop.
    g_stop_all = [&all_loops, &loops_mutex]() {
        std::lock_guard<std::mutex> lk(loops_mutex);
        for (auto* l : all_loops) l->stop();
    };
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Worker threads (cores 1..N-1)
    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (unsigned i = 1; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            core::EventLoop loop;
            {
                std::lock_guard<std::mutex> lk(loops_mutex);
                all_loops.push_back(&loop);
            }
            // Each TcpServer binds to the same port via SO_REUSEPORT; the
            // kernel load-balances incoming connections across all loops.
            core::TcpServer server(host, port, loop, dispatch);
            loop.run();
        });
    }

    // Main thread (core 0)
    core::EventLoop main_loop;
    {
        std::lock_guard<std::mutex> lk(loops_mutex);
        all_loops.push_back(&main_loop);
    }
    core::TcpServer main_server(host, port, main_loop, dispatch);

    std::cout << " * Osodio running on http://" << host << ":" << port
              << "  (threads=" << num_threads << ", press CTRL+C to quit)\n";

    main_loop.run();

    for (auto& t : threads) t.join();

    g_stop_all = nullptr;
}

} // namespace osodio
