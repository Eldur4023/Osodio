#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <unistd.h>
#include "types.hpp"
#include "router.hpp"
#include "openapi.hpp"
#include "di.hpp"
#include "group.hpp"
#include "websocket.hpp"
#include "metrics.hpp"

namespace osodio {

class App {
public:
    App()  = default;
    ~App() = default;

    // Route registration — support both :param and {param} styles.
    // Each registration also captures compile-time type info for OpenAPI generation.
    template<typename F> App& get   (std::string path, F&& h) { register_route("GET",    path, h); router_.add("GET",    std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& post  (std::string path, F&& h) { register_route("POST",   path, h); router_.add("POST",   std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& put   (std::string path, F&& h) { register_route("PUT",    path, h); router_.add("PUT",    std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& patch (std::string path, F&& h) { register_route("PATCH",  path, h); router_.add("PATCH",  std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& del   (std::string path, F&& h) { register_route("DELETE", path, h); router_.add("DELETE", std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& any   (std::string path, F&& h) {                                     router_.add("*",      std::move(path), std::forward<F>(h)); return *this; }

    // Middleware (applied in order for every request)
    App& use(Middleware m) { middlewares_.push_back(std::move(m)); return *this; }

    // Serve a directory of static files under a URL prefix.
    //   app.serve_static("/static", "./public")
    //   GET /static/app.js  →  ./public/app.js
    //
    // SPA fallback (spa = true): any path that doesn't match a real file is
    // served as ./public/index.html with status 200.  Enables client-side
    // routing for React/Vue/Svelte apps.
    //   app.serve_static("/", "./dist", true)
    App& serve_static(std::string url_prefix, std::string fs_root, bool spa = false) {
        static_mounts_.push_back({std::move(url_prefix), std::move(fs_root), spa});
        return *this;
    }

    // Register an error handler for a specific HTTP status code.
    //   app.on_error(404, [](int code, Request& req, Response& res) { ... });
    // Or a catch-all for any error (4xx/5xx):
    //   app.on_error([](int code, Request& req, Response& res) { ... });
    App& on_error(int code, ErrorHandler h) {
        error_handlers_[code] = std::move(h);
        return *this;
    }
    App& on_error(ErrorHandler h) {
        catchall_error_handler_ = std::move(h);
        return *this;
    }

    // Directorio donde se buscan los templates (default: "./templates")
    App& set_templates(std::string dir) { templates_dir_ = std::move(dir); return *this; }

    // Override the title and version shown in /docs and /openapi.json.
    App& api_info(std::string title, std::string version = "0.1.0") {
        api_title_   = std::move(title);
        api_version_ = std::move(version);
        return *this;
    }

    // ── OpenAPI / Swagger UI ─────────────────────────────────────────────────
    //
    // Opt-in: call enable_docs() to expose the spec and the Swagger UI.
    //
    //   app.enable_docs();                  // /openapi.json + /docs
    //   app.enable_docs("/api.json", "/ui"); // custom paths
    //
    App& enable_docs(std::string spec_path = "/openapi.json",
                     std::string ui_path   = "/docs") {
        openapi_spec_path_ = std::move(spec_path);
        openapi_ui_path_   = std::move(ui_path);
        openapi_enabled_   = true;
        return *this;
    }

    // ── Dependency injection ─────────────────────────────────────────────────
    //
    // Register a singleton — the same shared_ptr is returned for every request.
    //   app.provide(std::make_shared<Database>(conn_str));
    //
    template<typename T>
    App& provide(std::shared_ptr<T> instance) {
        container_.singleton<T>(std::move(instance));
        return *this;
    }

    // Register a transient factory — called once per Inject<T> resolution.
    //   app.provide<Logger>([]{ return std::make_shared<Logger>(); });
    //
    template<typename T, typename F>
    App& provide(F&& factory) {
        container_.transient<T>(std::forward<F>(factory));
        return *this;
    }

    // ── Route groups ─────────────────────────────────────────────────────────
    //
    // Creates a group with a URL prefix. Routes registered on the group are
    // prefixed automatically. Middleware added via group.use() runs only for
    // routes in that group, after global middlewares.
    //
    //   auto api = app.group("/api/v1");
    //   api.use(auth);
    //   api.get("/users", list_users);   // → GET /api/v1/users
    //
    RouteGroup group(std::string prefix) {
        return RouteGroup(std::move(prefix), router_, openapi_routes_);
    }

    // ── WebSocket ────────────────────────────────────────────────────────────
    //
    // Register a WebSocket handler.  The framework performs the RFC 6455
    // handshake and hands a WSConnection to the handler.
    //
    //   app.ws("/chat", [](WSConnection ws) -> Task<void> {
    //       while (ws.is_open()) {
    //           auto msg = co_await ws.recv();
    //           if (!msg) break;
    //           ws.send("echo: " + msg->data);
    //       }
    //   });
    //
    template<typename F>
    App& ws(std::string path, F&& fn) {
        auto wrapper = [fn = std::forward<F>(fn)](Request& req, Response& res) mutable -> Task<void> {
            // ── Validate upgrade headers ─────────────────────────────────────
            auto upgrade = req.header("upgrade");
            auto key     = req.header("sec-websocket-key");
            if (!upgrade || upgrade->find("websocket") == std::string::npos || !key) {
                res.status(426).json({{"error","WebSocket upgrade required"}});
                co_return;
            }

            // ── Send 101 Switching Protocols ─────────────────────────────────
            if (req._conn_fd < 0) {
                res.status(500).json({{"error","no fd for WS upgrade"}});
                co_return;
            }
            std::string hs =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + detail::ws_accept(*key) + "\r\n\r\n";
            size_t sent = 0;
            while (sent < hs.size()) {
                ssize_t n = ::write(req._conn_fd, hs.data()+sent, hs.size()-sent);
                if (n <= 0) co_return;
                sent += n;
            }

            // ── Create WSState and hook into the connection's read path ──────
            auto ws_state = std::make_shared<detail::WSState>();
            ws_state->fd    = req._conn_fd;
            ws_state->token = req.cancel_token;
            ws_state->loop  = req.loop;

            req._ws_on_readable = [ws_state]() { ws_state->on_readable(); };
            res.mark_ws_started();   // tell finish_dispatch: don't send response

            // ── Run user handler ─────────────────────────────────────────────
            WSConnection ws_conn(ws_state);
            co_await fn(std::move(ws_conn));
        };
        router_.add("GET", std::move(path), std::move(wrapper));
        return *this;
    }

    // ── Operational endpoints ────────────────────────────────────────────────
    //
    //   app.enable_health();    // GET /health  → JSON {status, uptime, …}
    //   app.enable_metrics();   // GET /metrics → Prometheus text
    //
    App& enable_health(std::string path = "/health") {
        router_.add("GET", std::move(path), [](Request&, Response& res) {
            res.json(Metrics::instance().to_health_json());
        });
        return *this;
    }

    App& enable_metrics(std::string path = "/metrics") {
        router_.add("GET", std::move(path), [](Request&, Response& res) {
            res.header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
               .send(Metrics::instance().to_prometheus());
        });
        return *this;
    }

    // Maximum simultaneous open connections (default 10 000).
    // Excess connections receive 503 immediately.
    App& max_connections(int n) { max_connections_ = n; return *this; }

    // Start listening — Flask style:
    //   app.run()               → 0.0.0.0:5000
    //   app.run(8080)           → 0.0.0.0:8080
    //   app.run("127.0.0.1", 8080)
    void run(const std::string& host = "0.0.0.0", uint16_t port = 5000);
    void run(uint16_t port) { run("0.0.0.0", port); }

    struct StaticMount { std::string prefix; std::string root; bool spa = false; };

private:
    // Called once per route registration to capture type metadata for OpenAPI.
    template<typename F>
    void register_route(const std::string& method, const std::string& path, const F&) {
        openapi_routes_.push_back(DocBuilder<std::decay_t<F>>::build(method, path));
    }

    Router                                    router_;
    std::vector<Middleware>                   middlewares_;
    std::vector<StaticMount>                  static_mounts_;
    std::unordered_map<int, ErrorHandler>     error_handlers_;
    ErrorHandler                              catchall_error_handler_;
    std::string                               templates_dir_ = "./templates";

    // OpenAPI state
    std::vector<RouteDoc>                     openapi_routes_;
    std::string                               api_title_       = "Osodio API";
    std::string                               api_version_     = "0.1.0";
    bool                                      openapi_enabled_ = false;
    std::string                               openapi_spec_path_ = "/openapi.json";
    std::string                               openapi_ui_path_   = "/docs";

    // Service container — populated before run(), read-only after
    ServiceContainer                          container_;

    int                                       max_connections_ = 10'000;
};

} // namespace osodio
