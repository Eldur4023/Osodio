#pragma once
#include <string>
#include <vector>
#include "types.hpp"
#include "router.hpp"
#include "openapi.hpp"
#include "handler_traits.hpp"

namespace osodio {

// ─── RouteGroup ───────────────────────────────────────────────────────────────
//
// A route group shares a URL prefix and an optional middleware chain that runs
// after the global middlewares but before each handler in the group.
//
//   auto api = app.group("/api/v1");
//   api.use(auth_middleware);
//
//   auto users = api.group("/users");   // inherits auth_middleware
//   users.get("/:id",  get_user);       // → GET /api/v1/users/:id
//   users.post("/",    create_user);    // → POST /api/v1/users
//
// Nested groups inherit the parent's middleware stack (copied at creation time).
//
// RouteGroup is a lightweight value type — safe to copy, move, and store.
// All route registrations happen at construction/startup time, before run().

class RouteGroup {
public:
    RouteGroup(std::string prefix,
               Router& router,
               std::vector<RouteDoc>& openapi_routes,
               std::vector<Middleware> inherited_middlewares = {})
        : prefix_(std::move(prefix))
        , router_(router)
        , openapi_routes_(openapi_routes)
        , middlewares_(std::move(inherited_middlewares))
    {}

    // ── Middleware ─────────────────────────────────────────────────────────────
    RouteGroup& use(Middleware m) {
        middlewares_.push_back(std::move(m));
        return *this;
    }

    // ── Route registration ─────────────────────────────────────────────────────
    template<typename F> RouteGroup& get   (std::string p, F&& h) { add("GET",    p, std::forward<F>(h)); return *this; }
    template<typename F> RouteGroup& post  (std::string p, F&& h) { add("POST",   p, std::forward<F>(h)); return *this; }
    template<typename F> RouteGroup& put   (std::string p, F&& h) { add("PUT",    p, std::forward<F>(h)); return *this; }
    template<typename F> RouteGroup& patch (std::string p, F&& h) { add("PATCH",  p, std::forward<F>(h)); return *this; }
    template<typename F> RouteGroup& del   (std::string p, F&& h) { add("DELETE", p, std::forward<F>(h)); return *this; }

    // ── Nested groups ──────────────────────────────────────────────────────────
    // Inherits this group's prefix + middlewares.
    RouteGroup group(std::string subprefix) {
        return RouteGroup(prefix_ + subprefix, router_, openapi_routes_, middlewares_);
    }

private:
    template<typename F>
    void add(const std::string& method, const std::string& path, F&& h) {
        const std::string full_path = prefix_ + path;

        // Register in OpenAPI doc
        openapi_routes_.push_back(DocBuilder<std::decay_t<F>>::build(method, full_path));

        // Wrap the user handler with HandlerTraits (extracts params, handles return type)
        Handler base = [h = std::forward<F>(h)](Request& req, Response& res) mutable -> Task<void> {
            return HandlerTraits<std::decay_t<F>>::call(h, req, res);
        };

        router_.add_internal(method, full_path, wrap(std::move(base)));
    }

    // Wraps a base handler in this group's middleware chain.
    // If there are no group middlewares, returns the handler as-is.
    Handler wrap(Handler base) {
        if (middlewares_.empty()) return base;

        auto mws = middlewares_;  // snapshot at registration time
        return [base = std::move(base), mws = std::move(mws)]
               (Request& req, Response& res) mutable -> Task<void>
        {
            // Local call_next that chains through group middlewares → base handler
            std::function<Task<void>(size_t)> call_next;
            call_next = [&base, &mws, &req, &res, &call_next](size_t i) -> Task<void> {
                if (i < mws.size()) {
                    co_await mws[i](req, res,
                        [&call_next, i]() -> Task<void> { return call_next(i + 1); });
                } else {
                    co_await base(req, res);
                }
            };
            co_await call_next(0);
        };
    }

    std::string              prefix_;
    Router&                  router_;
    std::vector<RouteDoc>&   openapi_routes_;
    std::vector<Middleware>  middlewares_;
};

} // namespace osodio
