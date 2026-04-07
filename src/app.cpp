#include "../include/osodio/app.hpp"
#include "../include/osodio/request.hpp"
#include "../include/osodio/response.hpp"

#include <osodio/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <fstream>
#include <filesystem>

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

// Returns true and writes the response if a static mount covers this path.
static bool try_serve_static(
    const std::vector<App::StaticMount>& mounts,
    const std::string& req_path,
    Response& res)
{
    namespace fs = std::filesystem;
    for (const auto& m : mounts) {
        if (req_path.rfind(m.prefix, 0) != 0) continue;

        // Strip the prefix, resolve to a real path
        std::string rel = req_path.substr(m.prefix.size());
        if (rel.empty() || rel.front() != '/') rel = '/' + rel;

        fs::path file = fs::path(m.root) / rel.substr(1); // strip leading '/'

        // Prevent path traversal
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
        if (!f) {
            res.status(500).json({{"error", "Could not read file"}});
            return true;
        }

        std::string body{std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>()};
        const char* mime = mime_for_ext(canonical_file.extension().string());
        res.header("Content-Type", mime).send(std::move(body));
        return true;
    }
    return false;
}

} // anonymous namespace

namespace {
    core::EventLoop* g_loop = nullptr;
    void signal_handler(int) {
        std::cout << "\nShutting down..." << std::endl;
        if (g_loop) g_loop->stop();
    }
}

void App::run(const std::string& host, uint16_t port) {
    std::signal(SIGPIPE, SIG_IGN);

    core::EventLoop loop;
    g_loop = &loop;
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Construimos la función de dispatch que:
    //  1. Configura el templates_dir en la Response
    //  2. Comprueba si la ruta la cubre un mount de archivos estáticos
    //  3. Ejecuta la cadena de middlewares
    //  4. Al final de la cadena, busca la ruta en el router
    DispatchFn dispatch = [this, &loop](Request& req, Response& res) {
        req.loop = &loop;
        res.set_templates_dir(templates_dir_);

        // Static files bypass the middleware chain (like nginx would)
        if (req.method == "GET" || req.method == "HEAD") {
            if (try_serve_static(static_mounts_, req.path, res)) return;
        }

        // call_next(i) ejecuta el middleware i, o el router si ya no quedan
        auto call_next = std::make_shared<std::function<void(size_t)>>();
        *call_next = [this, &req, &res, call_next](size_t i) {
            if (i < middlewares_.size()) {
                middlewares_[i](req, res, [call_next, i]() {
                    (*call_next)(i + 1);
                });
            } else {
                auto match = router_.match(req.method, req.path);
                if (match.found) {
                    req.params = std::move(match.params);
                    match.handler(req, res);
                } else {
                    res.status(404).json({{"error", "Not Found"}, {"path", req.path}});
                }
            }
        };
        (*call_next)(0);

        // Run error handlers after the full chain if status is 4xx/5xx
        // (skip async responses — the handler chain will run after completion)
        if (!res.is_async() && res.status_code() >= 400) {
            int code = res.status_code();
            auto it = error_handlers_.find(code);
            if (it != error_handlers_.end()) {
                it->second(code, req, res);
            } else if (catchall_error_handler_) {
                catchall_error_handler_(code, req, res);
            }
        }
    };

    core::TcpServer server(host, port, loop, std::move(dispatch));

    std::cout << " * Osodio running on http://" << host << ":" << port
              << "  (Press CTRL+C to quit)\n";

    loop.run();
    g_loop = nullptr;
}

} // namespace osodio
