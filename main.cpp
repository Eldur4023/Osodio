#include <osodio/osodio.hpp>
#include <iostream>

using namespace osodio;

struct User {
    std::string name;
    int age;

    OSODIO_VALIDATE(User,
        check(name.size() > 0, "Name cannot be empty"),
        check(age >= 18, "Must be at least 18 years old")
    )
};
OSODIO_SCHEMA(User, name, age)

int main() {
    App app;

    // ── Built-in middleware ──────────────────────────────────────────────────
    app.use(osodio::logger());
    app.use(osodio::cors({
        .origins = {"http://localhost:5173", "http://localhost:3000"},
    }));

    // ── Custom middleware (must co_await next() to continue chain) ───────────
    app.use([](Request& req, Response& res, auto next) -> Task<void> {
        // Pre-handler work
        res.header("X-Powered-By", "Osodio");
        co_await next();
        // Post-handler work (runs after the handler finishes, even if async)
    });

    // ── Routes ───────────────────────────────────────────────────────────────

    app.get("/ping", [](Request&, Response& res) {
        res.json({{"status", "ok"}});
    });

    // Async handler — co_await a sleep before responding
    app.get("/async-ping", [](Request& req) -> Task<nlohmann::json> {
        co_await sleep(500, req.loop);
        co_return nlohmann::json{{"status", "async-ok"}, {"waited_ms", 500}};
    });

    // Path parameter (type-safe)
    app.get("/users/:id", [](PathParam<std::string, "id"> id) -> User {
        return User{id.value, 25};
    });

    // Body auto-deserialization + validation
    app.post("/users", [](Body<User> user) {
        return nlohmann::json{{"id", 1}, {"name", user->name}};
    });

    // ── Static files ─────────────────────────────────────────────────────────
    app.serve_static("/static", "./public");

    // ── Error handlers ────────────────────────────────────────────────────────
    app.on_error(404, [](int, Request& req, Response& res) {
        res.json({{"error", "Not Found"}, {"path", req.path}});
    });
    app.on_error([](int code, Request&, Response& res) {
        res.json({{"error", "Something went wrong"}, {"code", code}});
    });

    app.run(8080);
    return 0;
}
