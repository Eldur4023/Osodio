#include <osodio/osodio.hpp>
#include <iostream>

using namespace osodio;

struct User {
    std::string name;
    int age;
    OSODIO_SCHEMA(User, name, age)
    OSODIO_VALIDATE(
        check(name.size() > 0, "Name cannot be empty"),
        check(age >= 18, "Must be at least 18 years old")
    )
};

// PATCH body: todos los campos son opcionales
struct UserPatch {
    std::optional<std::string> name;
    std::optional<int> age;
    OSODIO_SCHEMA(UserPatch, name, age)
    OSODIO_OPTIONAL(name, age)
};

// ── Demo service injected via Inject<T> ─────────────────────────────────────
struct UserStore {
    // In a real app this would hold a DB connection pool, etc.
    std::vector<User> users = {
        User{"Alice", 30},
        User{"Bob",   25},
    };
};

int main() {
    App app;

    app.api_info("Demo API", "0.1.0");  // visible at /docs and /openapi.json

    // ── Dependency injection ─────────────────────────────────────────────────
    app.provide(std::make_shared<UserStore>());

    // ── Limits ───────────────────────────────────────────────────────────────
    app.max_connections(5'000);

    // ── Built-in middleware ──────────────────────────────────────────────────
    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::cors({
        .origins = {"http://localhost:5173", "http://localhost:3000"},
    }));

    app.use([](Request&, Response& res, auto next) -> Task<void> {
        res.header("X-Powered-By", "Osodio");
        co_await next();
    });

    // ── Routes ───────────────────────────────────────────────────────────────

    app.get("/ping", [](Request&, Response& res) {
        res.json({{"status", "ok"}});
    });

    app.get("/async-ping", []() -> Task<nlohmann::json> {
        co_await sleep(500);
        co_return nlohmann::json{{"status", "async-ok"}, {"waited_ms", 500}};
    });

    // Inject<UserStore>: resolved from container, 500 if not registered
    app.get("/users", [](Inject<UserStore> store) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& u : store->users)
            arr.push_back({{"name", u.name}, {"age", u.age}});
        return arr;
    });

    // throw osodio::not_found() → 404 JSON, no try/catch needed in handler
    app.get("/users/:id", [](PathParam<int, "id"> id,
                             Inject<UserStore> store) -> User {
        int idx = id.value - 1;
        if (idx < 0 || idx >= static_cast<int>(store->users.size()))
            throw osodio::not_found("User not found");
        return store->users[idx];
    });

    // ── Route group with prefix + middleware ─────────────────────────────────
    auto api = app.group("/api");
    api.use([](Request&, Response& res, auto next) -> Task<void> {
        res.header("X-API-Version", "1");
        co_await next();
    });

    api.get("/users", [](Inject<UserStore> store) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& u : store->users)
            arr.push_back({{"name", u.name}, {"age", u.age}});
        return arr;
    });

    // Query params with defaults: ?page=1&limit=10
    api.get("/users/search", [](Query<int, "page", "1"> page,
                                Query<int, "limit", "10"> limit,
                                Query<std::string, "q"> q) -> nlohmann::json {
        return {{"page", (int)page}, {"limit", (int)limit}, {"q", (std::string)q}};
    });

    // PATCH with optional fields — send only the fields you want to change
    api.patch("/users/:id", [](PathParam<int, "id"> id,
                                UserPatch patch,
                                Inject<UserStore> store) -> nlohmann::json {
        int idx = id.value - 1;
        if (idx < 0 || idx >= static_cast<int>(store->users.size()))
            throw osodio::not_found("User not found");
        auto& u = store->users[idx];
        if (patch.name) u.name = *patch.name;
        if (patch.age)  u.age  = *patch.age;
        return {{"name", u.name}, {"age", u.age}};
    });

    // Plain User — auto-extracted from body, no Body<> wrapper needed.
    // Validation (OSODIO_VALIDATE) and 422 errors work exactly the same.
    app.post("/users", [](User user, Inject<UserStore> store) {
        store->users.push_back(user);
        return nlohmann::json{{"id", store->users.size()}, {"name", user.name}};
    });

    // Template rendering with inja (Jinja2-compatible)
    app.set_templates("./templates");
    app.get("/hello", [](Request& req, Response& res) {
        res.render("hello.html", {{"name", req.query.count("name")
            ? req.query.at("name") : "World"}});
    });


    app.get("/", [](Request&, Response& res){
        res.render("/index.html");
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
