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
| **Optional fields** | `OSODIO_OPTIONAL` — fields absent from the body → `std::nullopt`; PATCH-friendly |
| **Validation** | `OSODIO_VALIDATE` + `check()` inside the struct; all violations → 422 automatically |
| **Async** | C++20 `Task<T>` coroutines, `co_await sleep(ms)`, `co_await Task<U>` chaining |
| **Cancellation** | `CancellationToken` in `Request`; `sleep()` exits early when the connection closes |
| **Route groups** | `app.group("/api/v1").use(auth)` — URL prefix + per-group middleware |
| **Middleware** | Global `app.use()` chain; `logger()`, `cors()`, `compress()` built-in |
| **Dependency injection** | `app.provide<T>(...)` / `Inject<T>` in any handler |
| **Error handlers** | `app.on_error(code, fn)` and catch-all; typed throws: `not_found()`, `bad_request()`, … |
| **Static files** | `app.serve_static(prefix, dir, spa)` — MIME, ETag, Cache-Control, 304, sendfile(2), SPA fallback |
| **SSE** | `osodio::make_sse(res, req)` — `text/event-stream`, named events, keepalive pings, auto-disconnect |
| **Multipart** | `osodio::parse_multipart(req)` — file uploads, field names, Content-Type per part |
| **Templates** | `res.render("page.html", data)` via inja (Jinja2-compatible) |
| **OpenAPI + Swagger** | `/openapi.json` and `/docs` auto-generated from handler signatures at startup |
| **Compression** | `compress()` — gzip via zlib, negotiated by `Accept-Encoding` |
| **Rate limiting** | `rate_limit({.requests=100, .window_seconds=60})` — fixed-window per IP or custom key |
| **Security headers** | `helmet()` — CSP, HSTS, X-Frame-Options, X-Content-Type-Options, Referrer-Policy |
| **HTTP/1.1** | Keep-alive, incremental llhttp parser, non-blocking writes, sendfile(2) for statics |
| **Security** | 5s Slowloris timeout per request, 30s handler timeout, connection limit, 16 MB cap |
| **Remote IP** | `req.remote_ip` — IPv4/IPv6 resolved via `getpeername()` |
| **Vendored deps** | 8 files in `third_party/` — no network access during cmake |

---

## Quick Start

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

struct CreateUser {
    std::string name;
    int         age;
    std::optional<std::string> email;
    OSODIO_SCHEMA(CreateUser, name, age, email)
    OSODIO_OPTIONAL(email)               // absent in body → std::nullopt
    OSODIO_VALIDATE(
        check(name.size() >= 2, "Name too short"),
        check(age >= 18,        "Must be 18+")
    )
};

struct DB { std::vector<CreateUser> users; };

int main() {
    App app;
    app.provide(std::make_shared<DB>());
    app.api_info("My API", "1.0.0");     // visible at /docs
    app.max_connections(5'000);

    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::cors({.origins = {"http://localhost:5173"}}));

    // Route group: all routes under /api with a shared header
    auto api = app.group("/api/v1");
    api.use([](Request&, Response& res, auto next) -> Task<void> {
        res.header("X-API-Version", "1");
        co_await next();
    });

    // Body auto-parsed and validated; 422 on failure
    api.post("/users", [](CreateUser user, Inject<DB> db) {
        db->users.push_back(user);
        return nlohmann::json{{"id", db->users.size()}, {"name", user.name}};
    });

    // PATCH: send only the fields you want to update
    api.patch("/users/:id", [](PathParam<int,"id"> id,
                                CreateUser patch, Inject<DB> db) {
        if (id.value < 1 || id.value > (int)db->users.size())
            throw osodio::not_found("User not found");
        db->users[id.value - 1].name = patch.name;
        return nlohmann::json{{"ok", true}};
    });

    // Query params with defaults
    api.get("/users", [](Query<int,"page","1"> page,
                          Query<int,"limit","20"> limit) -> nlohmann::json {
        return {{"page", (int)page}, {"limit", (int)limit}};
    });

    // Async — sleep() doesn't need req.loop; exits early if client disconnects
    app.get("/slow", [](Request& req) -> Task<nlohmann::json> {
        co_await sleep(500);
        if (req.is_cancelled()) co_return {};
        co_return nlohmann::json{{"done", true}};
    });

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
app.run(8080);
app.run("127.0.0.1", 3000);
app.run();                           // 0.0.0.0:5000

app.api_info("My API", "1.0.0");    // shown in /docs and /openapi.json
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

Patterns: `/users/:id`, `/users/{id}`, `/files/*`

### Route Groups

```cpp
auto api = app.group("/api/v1");
api.use(auth_middleware);

auto admin = api.group("/admin");    // inherits auth
admin.use(admin_only);
admin.get("/stats", handler);        // → GET /api/v1/admin/stats
```

### Handler Arguments

| Type | Source |
|------|--------|
| `Request&` | Current request |
| `Response&` | Current response |
| `PathParam<T, "name">` | URL segment `:name` → T |
| `Query<T, "name">` | `?name=value` → T; absent → `T{}` |
| `Query<T, "name", "default">` | absent → converted from `"default"` |
| `Inject<T>` | Service from container (500 if not registered) |
| Any `OSODIO_SCHEMA` struct | Request body — parsed, validated, 422 on failure |
| `Body<T>` | Same, explicit wrapper with `operator bool` |

Supported `T` for `PathParam` / `Query`: `int`, `long`, `float`, `double`, `bool`, `std::string`.

### Schemas

```cpp
struct Product {
    int         id;
    std::string name;
    double      price;
    std::optional<std::string> note;

    OSODIO_SCHEMA(Product, id, name, price, note)
    OSODIO_OPTIONAL(note)            // absent in body → nullopt
    OSODIO_VALIDATE(
        check(price > 0,       "Price must be positive"),
        check(name.size() > 0, "Name required")
    )
};
```

All three macros go **inside** the struct definition, after the fields.

### Response

```cpp
res.status(201)
res.json({{"key", "value"}})
res.html("page.html")               // load from templates dir
res.html("<h1>Hello</h1>")          // inline HTML
res.text("plain text")
res.send("raw body")
res.header("X-Custom", "value")
res.render("index.html", data)      // inja Jinja2 template
```

Handlers can also return a value — serialized to JSON automatically:

```cpp
app.get("/a", [](Response& res) { res.json({{"x",1}}); });
app.get("/b", []() { return nlohmann::json{{"x",1}}; });
app.get("/c", []() -> Product { return {1,"item",9.99}; }); // OSODIO_SCHEMA
```

### Async & Cancellation

```cpp
// sleep() uses a thread-local event loop — no req.loop needed
app.get("/slow", []() -> Task<nlohmann::json> {
    co_await sleep(100);
    co_return nlohmann::json{{"done", true}};
});

// Check cancellation in long-running handlers
app.get("/poll", [](Request& req) -> Task<nlohmann::json> {
    for (int i = 0; i < 60; ++i) {
        co_await sleep(1000);
        if (req.is_cancelled()) co_return {};  // client disconnected
        // ... do work ...
    }
    co_return nlohmann::json{{"cycles", 60}};
});
```

`sleep()` also exits early (without waiting the full duration) when the connection is closed, so coroutines don't linger as zombies.

### Middleware

```cpp
app.use([](Request& req, Response& res, auto next) -> Task<void> {
    // before
    co_await next();
    // after
});

app.use(osodio::logger());
app.use(osodio::compress());                           // gzip, negotiated
app.use(osodio::compress({.min_size=512, .level=9}));
app.use(osodio::cors({
    .origins     = {"https://app.example.com"},
    .credentials = true,
    .max_age     = 86400,
}));

app.use(osodio::helmet());
app.use(osodio::helmet({
    .csp          = "default-src 'self' https://cdn.example.com",
    .hsts_max_age = 31'536'000,
}));

app.use(osodio::rate_limit({ .requests = 60, .window_seconds = 60 }));
app.use(osodio::rate_limit({
    .requests = 1000,
    .key_fn   = [](const Request& r) {
        return r.header("x-api-key").value_or(r.remote_ip);
    },
}));
```

### Dependency Injection

```cpp
app.provide(std::make_shared<Database>(conn_str));          // singleton
app.provide<Logger>([]{ return std::make_shared<Logger>(); }); // transient

app.get("/users", [](Inject<Database> db) -> nlohmann::json {
    // db->query(...)
});
```

### Error Handling

```cpp
// Throw from any handler
throw osodio::not_found("User not found");       // 404
throw osodio::bad_request("Missing field");      // 400
throw osodio::unauthorized("Invalid token");     // 401
throw osodio::forbidden("No access");            // 403
throw osodio::conflict("Already exists");        // 409
throw osodio::unprocessable("Invalid data");     // 422
throw osodio::too_many_requests("Slow down");    // 429
throw osodio::internal_error("DB error");        // 500

// Register handlers
app.on_error(404, [](int, Request& req, Response& res) {
    res.json({{"error","Not Found"},{"path",req.path}});
});
app.on_error([](int code, Request&, Response& res) {
    res.json({{"error","Something went wrong"},{"code",code}});
});
```

### Static Files & SPA

```cpp
app.serve_static("/static", "./public");          // prefix → directory
app.serve_static("/", "./dist", true);            // SPA fallback: unknown paths → index.html

// Uses sendfile(2) — zero-copy, no user-space buffer
// ETag + Cache-Control auto-set; 304 for unchanged files
// Hashed filenames (e.g. app.abc123.js) → immutable for 1 year
```

### Server-Sent Events (SSE)

```cpp
app.get("/stream", [](Request& req, Response& res) -> Task<void> {
    auto sse = osodio::make_sse(res, req);   // writes headers immediately

    int n = 0;
    while (sse.is_open()) {
        sse.send(std::to_string(n++));                     // data: N\n\n
        sse.send_event("tick", "payload", "evt-42");       // named event + id
        sse.ping();                                        // keepalive comment
        co_await osodio::sleep(1000);
    }
});
```

- `is_open()` returns false when the client disconnects (via `CancellationToken`)
- `sleep()` exits early on disconnect so the loop doesn't linger
- Browser reconnects automatically using `Last-Event-ID`

### Multipart / File Uploads

```cpp
app.post("/upload", [](Request& req, Response& res) -> Task<void> {
    auto parts = osodio::parse_multipart(req);
    if (!parts) { res.status(400).json({{"error","not multipart"}}); co_return; }

    for (auto& p : *parts) {
        if (!p.filename.empty()) {
            // file field: p.name, p.filename, p.content_type, p.body (bytes)
            std::ofstream f("uploads/" + p.filename, std::ios::binary);
            f.write(p.body.data(), p.body.size());
        } else {
            // text field: p.name, p.body
        }
    }
    res.json({{"uploaded", (int)parts->size()}});
});
```

### OpenAPI / Swagger

```cpp
app.api_info("My API", "1.0.0");
// GET /openapi.json  — OpenAPI 3.0 spec
// GET /docs          — Swagger UI
```

---

## Building

Requires **CMake 3.20+**, **C++20** (GCC 11+ or Clang 13+), **Linux** (epoll), **zlib** (system).

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
./build/example
```

All dependencies vendored in `third_party/` — no network during cmake:

```
third_party/
  nlohmann/json.hpp     nlohmann/json v3.11.3
  simdjson.h / .cpp     simdjson v3.10.0, amalgamated
  inja.hpp              inja v3.4.0, single-include
  llhttp/               llhttp v9.2.1 — 1 header + 3 .c files
```

---

## Status

| Feature | |
|---------|--|
| Radix tree router (`:param`, `{param}`, `*`) | ✅ |
| Route groups with per-group middleware | ✅ |
| Handler dependency injection (all param types) | ✅ |
| `OSODIO_SCHEMA` — body auto-extract, no wrapper | ✅ |
| `OSODIO_OPTIONAL` — `std::optional<T>` fields | ✅ |
| `OSODIO_VALIDATE` + `check()` | ✅ |
| `PathParam<T, "name">` | ✅ |
| `Query<T, "name", "default">` with default values | ✅ |
| `Inject<T>` — singleton + transient DI | ✅ |
| `Body<T>` — explicit body wrapper | ✅ |
| Typed HTTP errors (`not_found()`, `bad_request()`, …) | ✅ |
| C++20 `Task<T>` coroutines | ✅ |
| `co_await sleep(ms)` — no `req.loop` needed | ✅ |
| `CancellationToken` — sleep exits early on disconnect | ✅ |
| epoll event loop, non-blocking I/O, EPOLLOUT backpressure | ✅ |
| HTTP/1.1 keep-alive | ✅ |
| Header timeout 5s per request, re-armed on keep-alive (Slowloris) | ✅ |
| Handler + write timeout 30s | ✅ |
| Connection limit (`app.max_connections`) | ✅ |
| Response size cap 16 MB | ✅ |
| `compress()` — gzip, negotiated via Accept-Encoding | ✅ |
| `cors()` — full preflight handling | ✅ |
| `logger()` | ✅ |
| Static files — MIME, ETag, Cache-Control, 304, sendfile(2) | ✅ |
| SPA fallback — `serve_static(..., true)` → unknown paths → index.html | ✅ |
| Server-Sent Events — `make_sse()`, named events, ping, auto-disconnect | ✅ |
| Multipart/form-data — `parse_multipart()`, file + text fields | ✅ |
| Rate limiting — fixed-window per IP or custom key | ✅ |
| `helmet()` — CSP, HSTS, X-Frame-Options, X-Content-Type-Options | ✅ |
| `req.remote_ip` — IPv4/IPv6 from `getpeername()` | ✅ |
| HTML templates via inja (Jinja2-compatible) | ✅ |
| OpenAPI 3.0 + Swagger UI at `/docs` | ✅ |
| Global error handlers | ✅ |
| Vendored deps — 8 files, no cmake network | ✅ |
| WebSockets | ⬜ |
| HTTPS / TLS | ⬜ |
| HTTP/2 | ⬜ |
| Brotli compression | ⬜ |
| Multi-thread (one loop per core) | ⬜ |
