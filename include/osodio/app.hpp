#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "types.hpp"
#include "router.hpp"
#include "openapi.hpp"
#include "di.hpp"

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
    App& serve_static(std::string url_prefix, std::string fs_root) {
        static_mounts_.push_back({std::move(url_prefix), std::move(fs_root)});
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

    // Start listening — Flask style:
    //   app.run()               → 0.0.0.0:5000
    //   app.run(8080)           → 0.0.0.0:8080
    //   app.run("127.0.0.1", 8080)
    void run(const std::string& host = "0.0.0.0", uint16_t port = 5000);
    void run(uint16_t port) { run("0.0.0.0", port); }

    struct StaticMount { std::string prefix; std::string root; };

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
    std::string                               api_title_   = "Osodio API";
    std::string                               api_version_ = "0.1.0";

    // Service container — populated before run(), read-only after
    ServiceContainer                          container_;
};

} // namespace osodio
