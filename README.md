# Osodio

C++20 web framework with FastAPI/Flask ergonomics. epoll event loop, C++20 coroutines, zero network dependencies.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

struct User {
    std::string name;
    int age;
    OSODIO_SCHEMA(User, name, age)
    OSODIO_VALIDATE(
        check(name.size() > 0, "Name required"),
        check(age >= 18, "Must be 18+")
    )
};

int main() {
    App app;

    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::cors({.origins = {"http://localhost:5173"}}));

    app.post("/users", [](User user) {
        return nlohmann::json{{"name", user.name}, {"age", user.age}};
    });

    app.get("/users/:id", [](PathParam<int, "id"> id) -> Task<nlohmann::json> {
        co_await sleep(10);
        co_return nlohmann::json{{"id", (int)id}};
    });

    app.run(8080);
}
```

---

## Features

| | |
|---|---|
| **Routing** | Radix tree, `:param` and `{param}` styles, `*` wildcards |
| **Handler injection** | `PathParam`, `Query`, body structs, `Inject<T>`, `Request&`, `Response&` — auto-extracted from signature |
| **Body parsing** | Any `OSODIO_SCHEMA` struct as a bare parameter — no `Body<>` wrapper needed |
| **Optional fields** | `OSODIO_OPTIONAL` marks fields that may be absent in the body (PATCH-friendly) |
| **Validation** | `OSODIO_VALIDATE` + `check()` inside the struct; violations → 422 automatically |
| **Async** | C++20 `Task<T>` coroutines, `co_await sleep(ms)`, `co_await Task<U>` chaining |
| **Route groups** | `app.group("/api/v1").use(auth)` — prefix + per-group middleware |
| **Middleware** | Global `app.use()` chain and per-group; ordered `next()` continuation |
| **Dependency injection** | `app.provide<T>(...)` / `Inject<T>` in handler params |
| **Error handlers** | `app.on_error(code, fn)` and catch-all |
| **Static files** | `app.serve_static(prefix, dir)` with MIME, ETag, Cache-Control, 304 support |
| **Templates** | `res.render("page.html", data)` via inja (Jinja2-compatible) |
| **OpenAPI + Swagger** | `/openapi.json` and `/docs` (Swagger UI) auto-generated at startup |
| **HTTP/1.1** | Keep-alive, incremental llhttp parser, non-blocking writes, backpressure |
| **Security** | Slowloris protection (5s header timeout), connection limit, path traversal blocked |
| **Compression** | `osodio::compress()` middleware — gzip, negotiated via Accept-Encoding |
| **Vendored deps** | 8 files in `third_party/` — no network on cmake |

---

## Quick Start

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

// ── Schema + validation inside the struct ────────────────────────────────────
struct CreateUser {
    std::string name;
    int         age;
    std::optional<std::string> email;  // optional field
    OSODIO_SCHEMA(CreateUser, name, age, email)
    OSODIO_OPTIONAL(email)
    OSODIO_VALIDATE(
        check(name.size() >= 2,  "Name too short"),
        check(age >= 18,         "Must be 18+")
    )
};

// ── Service injected via Inject<T> ───────────────────────────────────────────
struct DB {
    std::vector<CreateUser> users;
};

int main() {
    App app;
    app.provide(std::make_shared<DB>());
    app.api_info("My API", "1.0.0");   // visible at /docs
    app.max_connections(5'000);

    // Global middleware
    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::cors({.origins = {"http://localhost:5173"}}));

    // Route group with its own middleware
    auto api = app.group("/api/v1");
    api.use([](Request&, Response& res, auto next) -> Task<void> {
        res.header("X-API-Version", "1");
        co_await next();
    });

    // Body auto-extracted and validated → 422 on failure
    api.post("/users", [](CreateUser user, Inject<DB> db) {
        db->users.push_back(user);
        return nlohmann::json{{"id", db->users.size()}, {"name", user.name}};
    });

    // PATCH with optional fields — only send what you want to change
    api.patch("/users/:id", [](PathParam<int,"id"> id,
                                CreateUser patch, Inject<DB> db) {
        if (id.value < 1 || id.value > (int)db->users.size())
            throw osodio::not_found("User not found");
        auto& u = db->users[id.value - 1];
        u.name = patch.name;
        return nlohmann::json{{"name", u.name}};
    });

    // Query params with defaults
    api.get("/users", [](Query<int,"page","1"> page,
                          Query<int,"limit","20"> limit,
                          Inject<DB> db) -> nlohmann::json {
        return {{"page", (int)page}, {"limit", (int)limit},
                {"total", db->users.size()}};
    });

    // Async handler — sleep doesn't block the event loop
    app.get("/slow", []() -> Task<nlohmann::json> {
        co_await sleep(200);
        co_return nlohmann::json{{"done", true}};
    });

    // Error handlers
    app.on_error(404, [](int, Request& req, Response& res) {
        res.json({{"error", "Not Found"}, {"path", req.path}});
    });

    app.run(8080);
}
```

---

## API Reference

### App

```cpp
App app;
app.run(8080);                       // 0.0.0.0:8080
app.run("127.0.0.1", 3000);
app.run();                           // 0.0.0.0:5000 (default)

app.api_info("My API", "1.0.0");    // shown in /docs
app.max_connections(10'000);         // 503 beyond this limit (default: 10 000)
app.set_templates("./views");        // template root (default: ./templates)
```

### Routing

```cpp
app.get   ("/path", handler);
app.post  ("/path", handler);
app.put   ("/path", handler);
app.patch ("/path", handler);
app.del   ("/path", handler);
app.any   ("/path", handler);        // all methods
```

Route patterns:
- `/users/:id` — named parameter → `PathParam<T, "id">`
- `/users/{id}` — same, `{}` syntax also supported
- `/files/*` — wildcard, matches everything after `/files/`

### Route Groups

```cpp
auto api = app.group("/api/v1");
api.use(auth_middleware);            // runs only for routes in this group

auto admin = api.group("/admin");    // inherits api's middleware
admin.use(admin_only_middleware);    // adds on top
admin.get("/stats", get_stats);     // → GET /api/v1/admin/stats
```

### Handler Arguments

Any combination, any order — resolved from the lambda signature automatically.

| Type | Source |
|------|--------|
| `Request&` | Current request |
| `Response&` | Current response |
| `PathParam<T, "name">` | URL segment `:name` → converted to `T` |
| `Query<T, "name">` | `?name=value` → `T`; absent → `T{}` |
| `Query<T, "name", "default">` | same, absent → converted from `"default"` |
| `Inject<T>` | Service resolved from container (500 if not registered) |
| Any `OSODIO_SCHEMA` struct | Request body, parsed + validated automatically |
| `Body<T>` | Same as above, explicit wrapper with `operator bool` |

Supported `T` for `PathParam` / `Query`: `int`, `long`, `float`, `double`, `bool`, `std::string`.

### Defining Schemas

```cpp
struct Product {
    int         id;
    std::string name;
    double      price;
    std::optional<std::string> description;  // may be absent in body

    OSODIO_SCHEMA(Product, id, name, price, description)
    OSODIO_OPTIONAL(description)             // absence allowed; null → std::nullopt
    OSODIO_VALIDATE(
        check(price > 0,      "Price must be positive"),
        check(name.size() > 0, "Name required")
    )
};
```

- `OSODIO_SCHEMA` — generates `from_json` / `to_json`. Place inside the struct.
- `OSODIO_OPTIONAL` — fields that may be missing from the request body. Must be `std::optional<T>`.
- `OSODIO_VALIDATE` — business rules. `check(cond, msg)` adds to error list on failure.
- Missing required fields → 422. Failed `check` → 422 with all messages at once.

### Response

```cpp
res.status(201)
res.json({{"key", "value"}})
res.html("page.html")               // load from templates dir
res.html("<h1>Hello</h1>")          // inline HTML
res.text("plain text")
res.send("raw body")
res.header("X-Custom", "value")
res.render("index.html", data)      // inja template with JSON data
```

Handlers can also return a value — it's serialized to JSON automatically:

```cpp
app.get("/a", [](Response& res) { res.json({{"x",1}}); });   // explicit
app.get("/b", []() { return nlohmann::json{{"x",1}}; });      // return value
app.get("/c", []() -> Product { return {1,"item",9.99}; });   // OSODIO_SCHEMA struct
```

### Async

```cpp
app.get("/slow", []() -> Task<nlohmann::json> {
    co_await sleep(100);                      // non-blocking, no req.loop needed
    co_return nlohmann::json{{"done", true}};
});

// Chain tasks
Task<std::string> fetch_data() {
    co_await sleep(50);
    co_return std::string{"result"};
}

app.get("/chain", []() -> Task<nlohmann::json> {
    auto s = co_await fetch_data();
    co_return nlohmann::json{{"value", s}};
});
```

### Middleware

```cpp
// Global
app.use([](Request& req, Response& res, auto next) -> Task<void> {
    // before handler
    co_await next();
    // after handler
});

// Built-in
app.use(osodio::logger());
app.use(osodio::compress());                          // gzip, negotiated
app.use(osodio::compress({.min_size=512, .level=9})); // options
app.use(osodio::cors({
    .origins     = {"https://app.example.com"},
    .credentials = true,
    .max_age     = 86400,
}));
```

### Dependency Injection

```cpp
// Register at startup
app.provide(std::make_shared<Database>(conn_str));   // singleton
app.provide<Logger>([]{ return std::make_shared<Logger>(); }); // transient

// Use in any handler
app.get("/users", [](Inject<Database> db) -> nlohmann::json {
    // db->query(...)
});
```

### Error Handling

```cpp
// Throw from any handler — caught automatically
throw osodio::not_found("User not found");       // 404
throw osodio::bad_request("Missing field");      // 400
throw osodio::unauthorized("Invalid token");     // 401
throw osodio::forbidden("No access");            // 403
throw osodio::conflict("Already exists");        // 409
throw osodio::unprocessable("Invalid data");     // 422
throw osodio::too_many_requests("Slow down");    // 429
throw osodio::internal_error("DB error");        // 500

// Register error handlers
app.on_error(404, [](int, Request& req, Response& res) {
    res.json({{"error","Not Found"},{"path",req.path}});
});
app.on_error([](int code, Request&, Response& res) {
    res.json({{"error","Something went wrong"},{"code",code}});
});
```

### OpenAPI / Swagger

```cpp
app.api_info("My API", "1.0.0");
// Then visit:
//   GET /openapi.json  — OpenAPI 3.0 spec
//   GET /docs          — Swagger UI
```

---

## Building

Requires **CMake 3.20+**, **C++20** compiler (GCC 11+ or Clang 13+), **Linux** (epoll), **zlib** (system).

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
./build/example
```

All dependencies are vendored in `third_party/` (8 files total) — no network access during cmake.

```
third_party/
  nlohmann/json.hpp   — JSON (v3.11.3)
  simdjson.h/.cpp     — fast JSON parser, amalgamated (v3.10.0)
  inja.hpp            — Jinja2 templates, single-include (v3.4.0)
  llhttp/             — HTTP/1.1 parser from Node.js (v9.2.1)
```

To use Osodio in your own project:

```cmake
add_subdirectory(osodio)
target_link_libraries(myapp PRIVATE osodio)
```

---

## Project Status

| Feature | Status |
|---------|--------|
| Radix tree router (`:param`, `*`, `{}`) | ✅ Done |
| Route groups with per-group middleware | ✅ Done |
| Handler dependency injection (all param types) | ✅ Done |
| `OSODIO_SCHEMA` — body auto-extract, no wrapper needed | ✅ Done |
| `OSODIO_OPTIONAL` — optional fields, `std::optional<T>` support | ✅ Done |
| `OSODIO_VALIDATE` + `check()` | ✅ Done |
| `PathParam<T, "name">` | ✅ Done |
| `Query<T, "name", "default">` — with default values | ✅ Done |
| `Inject<T>` — singleton + transient DI | ✅ Done |
| `Body<T>` — explicit body wrapper | ✅ Done |
| Typed HTTP errors (`not_found()`, `bad_request()`, …) | ✅ Done |
| C++20 `Task<T>` coroutines | ✅ Done |
| `co_await sleep(ms)` — no `req.loop` needed | ✅ Done |
| epoll event loop, non-blocking I/O | ✅ Done |
| HTTP/1.1 keep-alive, backpressure (EPOLLOUT) | ✅ Done |
| Slowloris protection (5s header timeout) | ✅ Done |
| Request timeout (30s handler + write) | ✅ Done |
| Connection limit (`max_connections`) | ✅ Done |
| `compress()` middleware — gzip | ✅ Done |
| `cors()` middleware — full preflight | ✅ Done |
| `logger()` middleware | ✅ Done |
| Static files — MIME, ETag, Cache-Control, 304 | ✅ Done |
| HTML templates via inja (Jinja2-compatible) | ✅ Done |
| OpenAPI 3.0 + Swagger UI at `/docs` | ✅ Done |
| Global error handlers | ✅ Done |
| Vendored deps (8 files, no cmake network) | ✅ Done |
| write_buf copy-free (offset, no substr) | ✅ Done |
| Backpressure buffer limit (OOM protection) | 🔴 Pending |
| Coroutine cancellation (zombie tasks) | 🔴 Pending |
| WebSockets | ⬜ Not started |
| Server-Sent Events (SSE) | ⬜ Not started |
| Multipart / form-data | ⬜ Not started |
| HTTPS / TLS | ⬜ Not started |
| HTTP/2 | ⬜ Not started |
| Rate limiting middleware | ⬜ Not started |
| `helmet()` security headers middleware | ⬜ Not started |
| SPA fallback in static files | ⬜ Not started |
