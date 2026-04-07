# Osodio

A C++20 web framework with FastAPI/Flask ergonomics. Single header include, zero runtime dependencies, epoll-based event loop.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

int main() {
    App app;
    app.get("/hello", [](Request&, Response& res) {
        res.json({{"message", "Hello, world!"}});
    });
    app.run(8080);
}
```

---

## Features

| | |
|---|---|
| Routing | Radix tree, `:param` segments, `*` wildcards |
| Handler injection | `PathParam`, `Query`, `Body`, `Request&`, `Response&` auto-extracted from lambda signature |
| JSON | `OSODIO_SCHEMA` generates `to_json`/`from_json` via nlohmann/json |
| Validation | `OSODIO_VALIDATE` + `check()` inside structs; violations → 422 automatically |
| Async | C++20 `Task<T>` coroutines, `co_await sleep(ms)`, chained `co_await Task<U>` |
| Middleware | `app.use()` with ordered `next()` continuation |
| Static files | `app.serve_static(prefix, dir)` with MIME detection and path-traversal protection |
| Error handlers | `app.on_error(code, fn)` and catch-all `app.on_error(fn)` |
| Templates | `res.html("page.html")` loads from `./templates/` (configurable) |
| HTTP/1.1 | Keep-alive, incremental parser, `Content-Length` |
| No dependencies | nlohmann/json vendored in `third_party/` |

---

## Quick Start

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

struct User {
    std::string name;
    int age;

    OSODIO_VALIDATE(User,
        check(name.size() >= 2, "Name too short"),
        check(age >= 18,        "Must be 18 or older")
    )
};
OSODIO_SCHEMA(User, name, age)   // generates from_json / to_json

int main() {
    App app;

    // Middleware (runs before every route)
    app.use([](Request& req, Response&, auto next) {
        std::cout << req.method << " " << req.path << "\n";
        next();
    });

    // Static files: GET /static/* → ./public/*
    app.serve_static("/static", "./public");

    // Simple GET
    app.get("/ping", [](Request&, Response& res) {
        res.json({{"status", "ok"}});
    });

    // Path parameter
    app.get("/users/:id", [](PathParam<int, "id"> id, Response& res) {
        res.json({{"id", id.value}});
    });

    // Body parsing + automatic validation (returns 422 on failure)
    app.post("/users", [](Body<User> user) {
        return nlohmann::json{{"created", user->name}};
    });

    // Async coroutine
    app.get("/slow", [](Request& req) -> Task<nlohmann::json> {
        co_await sleep(200, req.loop);
        co_return nlohmann::json{{"done", true}};
    });

    // Custom error pages
    app.on_error(404, [](int, Request& req, Response& res) {
        res.html("404.html");
    });

    app.run(8080);
}
```

---

## API Reference

### App

```cpp
App app;
app.run(8080);                          // 0.0.0.0:8080
app.run("127.0.0.1", 3000);            // specific host
app.run();                              // 0.0.0.0:5000 (default)
```

#### Routing

```cpp
app.get   ("/path", handler);
app.post  ("/path", handler);
app.put   ("/path", handler);
app.patch ("/path", handler);
app.del   ("/path", handler);
app.any   ("/path", handler);           // all methods
```

Route patterns:
- `/users/:id` — named parameter, extracted as `PathParam<T, "id">`
- `/files/*` — wildcard, matches anything after `/files/`
- `/users/{id}` — `{id}` syntax also supported (converted to `:id`)

#### Middleware

```cpp
app.use([](Request& req, Response& res, auto next) {
    // runs before the handler
    next();
    // runs after the handler (if sync)
});
```

Middleware runs in registration order. Calling `next()` advances to the next middleware or the router. Not calling `next()` short-circuits the chain.

#### Static Files

```cpp
app.serve_static("/static", "./public");
// GET /static/app.js   → ./public/app.js
// GET /static/img/x.png → ./public/img/x.png
```

MIME types detected from extension: `.html`, `.css`, `.js`, `.json`, `.svg`, `.png`, `.jpg`, `.gif`, `.webp`, `.ico`, `.woff`, `.woff2`, `.ttf`, `.pdf`, `.xml`, `.txt`. Anything else → `application/octet-stream`. Path traversal (`../`) is blocked with 403.

Multiple mounts are supported; first match wins.

#### Error Handlers

```cpp
app.on_error(404, [](int code, Request& req, Response& res) {
    res.status(404).html("404.html");
});

app.on_error(500, [](int code, Request&, Response& res) {
    res.status(500).json({{"error", "Internal error"}});
});

// Catch-all (runs if no specific handler matches)
app.on_error([](int code, Request&, Response& res) {
    res.json({{"error", "Unexpected error"}, {"code", code}});
});
```

Error handlers run after the route handler (and any middleware). They only fire when `status_code >= 400`. The specific-code handler takes precedence over the catch-all.

#### Templates

```cpp
app.set_templates("./views");   // default: "./templates"

app.get("/", [](Request&, Response& res) {
    res.html("index.html");     // loads ./views/index.html
});
```

---

### Handler Arguments

Arguments are resolved automatically from the lambda signature — any combination, any order.

| Type | Resolves from |
|------|--------------|
| `Request&` | The current request |
| `Response&` | The current response |
| `PathParam<T, "name">` | URL segment `:name`, converted to `T` |
| `Query<T, "name">` | Query string `?name=value`, converted to `T` |
| `Body<T>` | Request body parsed as JSON into `T` |

Supported `T` for `PathParam` and `Query`: `int`, `long`, `float`, `double`, `std::string`.

```cpp
// All arguments are optional and independent
app.get("/items/:id", [](PathParam<int, "id"> id, Query<int, "limit"> limit) {
    // id.value, limit.value
});

app.post("/items", [](Body<Item> item, Response& res) {
    if (item->name.empty()) { res.status(400).json({{"error","bad"}}); return; }
    res.status(201).json({{"id", 1}});
});
```

---

### Response

```cpp
res.status(201)                          // set status code (chainable)
res.json({{"key", "value"}})            // Content-Type: application/json
res.html("page.html")                   // load template file
res.html("<h1>Hello</h1>")             // inline HTML string
res.text("plain text")                  // Content-Type: text/plain
res.send("raw body")                    // set body without Content-Type
res.header("X-Custom", "value")        // set arbitrary header
```

Handlers that take `Response&` use the builder API. Handlers that *return* a value have it serialized to JSON automatically:

```cpp
// These are equivalent:
app.get("/a", [](Response& res) { res.json({{"x", 1}}); });
app.get("/b", []() { return nlohmann::json{{"x", 1}}; });
app.get("/c", []() -> MyStruct { return {1, "hello"}; }); // needs OSODIO_SCHEMA
```

---

### Request

```cpp
req.method                          // "GET", "POST", etc.
req.path                            // "/users/42"
req.body                            // raw body string
req.headers                         // std::unordered_map (lowercase keys)
req.params                          // path params: params["id"]
req.query                           // query params: query["page"]
req.loop                            // event loop pointer (for coroutines)

req.header("content-type")         // std::optional<std::string>
req.query_param("page", "1")       // with default value
```

---

### JSON Serialization — OSODIO_SCHEMA

```cpp
struct Product {
    int         id;
    std::string name;
    double      price;
};
OSODIO_SCHEMA(Product, id, name, price)   // must be outside the struct
```

This generates `from_json` and `to_json` via nlohmann/json's `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`. Once defined:
- `Body<Product>` deserializes the request JSON automatically.
- Returning `Product` from a handler serializes it to JSON automatically.

---

### Validation — OSODIO_VALIDATE

```cpp
struct CreateUser {
    std::string email;
    std::string name;
    int         age;

    OSODIO_VALIDATE(CreateUser,
        check(name.size() >= 2,        "Name must be at least 2 characters"),
        check(age >= 18 && age <= 120,  "Age must be between 18 and 120"),
        check(email.find('@') != std::string::npos, "Invalid email")
    )
};
OSODIO_SCHEMA(CreateUser, email, name, age)
```

- `check(condition, message)` — adds `message` to the error list if `condition` is false.
- Multiple `check` calls are all evaluated; all failures are reported at once.
- When used with `Body<T>`, a failed validation automatically returns 422 with:
  ```json
  { "error": "Validation Failed", "messages": ["Name must be at least 2 characters"] }
  ```
- To call validation manually: `validate(obj)` — throws `osodio::ValidationError` on failure.

---

### Async — Task\<T\>

```cpp
app.get("/data", [](Request& req) -> Task<nlohmann::json> {
    co_await sleep(100, req.loop);             // non-blocking wait
    co_return nlohmann::json{{"data", "..."}};
});
```

- `Task<T>` is the coroutine return type.
- `co_await sleep(ms, req.loop)` suspends without blocking the event loop thread.
- Other handlers continue executing during the suspension.
- `req.loop` must be passed to `sleep()` and to any nested task that needs scheduling.

Chaining tasks:

```cpp
Task<std::string> fetch(core::EventLoop* loop) {
    co_await sleep(50, loop);
    co_return std::string("result");
}

app.get("/chain", [](Request& req) -> Task<nlohmann::json> {
    auto s = co_await fetch(req.loop);
    co_return nlohmann::json{{"value", s}};
});
```

`Task<void>` is also supported for fire-and-forget operations.

---

## Building

Requires **CMake 3.20+** and a **C++20** compiler (GCC 11+ or Clang 13+). Linux only (uses epoll).

```bash
# In-source (simplest)
cmake .
make

# Out-of-tree
cmake -S . -B build
make -C build

# Run the example
./example

# Clean build artifacts
make distclean
```

The `osodio` static library is built from `src/`. Link your own project against it:

```cmake
add_subdirectory(osodio)
target_link_libraries(myapp PRIVATE osodio)
```

---

## Project Status

| Feature | Status |
|---------|--------|
| Radix tree router (`:param`, `*`) | Done |
| Middleware (`app.use`) | Done |
| Handler dependency injection | Done |
| `OSODIO_SCHEMA` — auto JSON serialization | Done |
| `OSODIO_VALIDATE` + `check()` | Done |
| `Body<T>` with automatic validation | Done |
| `PathParam<T, "name">` | Done |
| `Query<T, "name">` | Done |
| C++20 `Task<T>` coroutines | Done |
| `co_await sleep(ms)` | Done |
| Chained `co_await Task<U>` | Done |
| HTML template serving | Done |
| Static file server (`app.serve_static`) | Done |
| Global error handlers (`app.on_error`) | Done |
| HTTP/1.1 keep-alive | Done |
| epoll event loop | Done |
| Vendored nlohmann/json | Done |
| HTTPS / TLS | Not started |
| Multipart / form-data | Not started |
| WebSockets | Not started |
| OpenAPI / Swagger generation | Not started |
