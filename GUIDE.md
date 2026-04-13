# Osodio — Complete Developer Guide

> **Purpose of this document:** comprehensive reference for developers and LLMs working with the Osodio C++20 web framework. Every public API is documented with working code examples. After reading this document an LLM should be able to write correct Osodio code without additional context.

---

## Table of Contents

1. [What is Osodio](#1-what-is-osodio)
2. [Quick Start](#2-quick-start)
3. [Routing](#3-routing)
4. [Handler Signatures](#4-handler-signatures)
5. [Request API](#5-request-api)
6. [Response API](#6-response-api)
7. [Path Parameters](#7-path-parameters)
8. [Query Parameters](#8-query-parameters)
9. [Request Body — SCHEMA](#9-request-body--schema)
10. [Validation — validate()](#10-validation--validate)
11. [Async Handlers — Task\<T\>](#11-async-handlers--taskt)
12. [Middleware](#12-middleware)
13. [Built-in Middleware](#13-built-in-middleware)
14. [Route Groups](#14-route-groups)
15. [Error Handling](#15-error-handling)
16. [Dependency Injection](#16-dependency-injection)
17. [JWT Authentication](#17-jwt-authentication)
18. [Server-Sent Events (SSE)](#18-server-sent-events-sse)
19. [WebSockets](#19-websockets)
20. [Static Files & SPA](#20-static-files--spa)
21. [HTML Templates](#21-html-templates)
22. [OpenAPI / Swagger UI](#22-openapi--swagger-ui)
23. [Health & Metrics](#23-health--metrics)
24. [TLS / HTTPS](#24-tls--https)
25. [HTTP/2](#25-http2)
26. [Multipart Uploads](#26-multipart-uploads)
27. [TestClient — In-Process Testing](#27-testclient--in-process-testing)
28. [Graceful Shutdown](#28-graceful-shutdown)
29. [CancellationToken](#29-cancellationtoken)
30. [Architecture Internals](#30-architecture-internals)

---

## 1. What is Osodio

Osodio is a C++20 web framework for Linux. Design goals:

- **DX first**: handler signatures look like typed function signatures, not HTTP boilerplate
- **Async-native**: built on C++20 coroutines (`co_await`, `co_return`), epoll event loop
- **Zero overhead abstractions**: `PathParam<int,"id">`, `Query<T,"name">`, `Body<T>`, `Inject<T>` are all resolved at compile time
- **Complete HTTP stack**: HTTP/1.1, HTTP/2 (nghttp2), TLS (OpenSSL), WebSocket (RFC 6455 + RFC 8441), SSE

**Single include:**
```cpp
#include <osodio/osodio.hpp>
```

**Minimal working server:**
```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

int main() {
    App app;
    app.get("/", [](Response& res) { res.json({{"hello","world"}}); });
    app.run(8080);
}
```

---

## 2. Quick Start

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(osodio)           # or find_package(osodio)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE osodio)
```

### Complete CRUD example

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

struct User {
    int         id;
    std::string name;
    int         age;
    SCHEMA(User, id, name, age)

    std::vector<std::string> validate() const {
        if (name.empty()) return {"name: required"};
        if (age < 0)      return {"age: must be non-negative"};
        return {};
    }
};

struct CreateUser {
    std::string name;
    int         age;
    SCHEMA(CreateUser, name, age)

    std::vector<std::string> validate() const {
        if (name.empty()) return {"name: required"};
        if (age < 0)      return {"age: must be non-negative"};
        return {};
    }
};

// In-memory store — in production, inject a real DB
struct UserStore {
    std::vector<User> users = {{1,"Alice",30},{2,"Bob",25}};
    int next_id = 3;
};

int main() {
    App app;
    app.provide(std::make_shared<UserStore>());

    app.get("/users", [](Inject<UserStore> s) {
        return nlohmann::json(s->users);
    });

    app.get("/users/:id", [](PathParam<int,"id"> id, Inject<UserStore> s) -> User {
        for (auto& u : s->users)
            if (u.id == id.value) return u;
        throw not_found("User not found");
    });

    app.post("/users", [](CreateUser body, Inject<UserStore> s) {
        User u{s->next_id++, body.name, body.age};
        s->users.push_back(u);
        return nlohmann::json{{"id", u.id}};
    });

    app.del("/users/:id", [](PathParam<int,"id"> id, Inject<UserStore> s,
                              Response& res) {
        auto& v = s->users;
        auto it = std::find_if(v.begin(), v.end(),
                               [&](auto& u){ return u.id == id.value; });
        if (it == v.end()) throw not_found();
        v.erase(it);
        res.status(204).send("");
    });

    app.run(8080);
}
```

---

## 3. Routing

### HTTP methods

```cpp
app.get   ("/path", handler);
app.post  ("/path", handler);
app.put   ("/path", handler);
app.patch ("/path", handler);
app.del   ("/path", handler);   // "delete" is reserved; use del
app.any   ("/path", handler);   // matches all methods
```

### Path parameter syntax

Both styles are equivalent:

```cpp
app.get("/users/:id",   handler);   // Express-style
app.get("/users/{id}",  handler);   // OpenAPI-style
```

### Wildcard routes

```cpp
app.get("/files/*", [](Request& req, Response& res) {
    // req.params["*"] contains the wildcard portion
    res.text("File: " + req.params["*"]);
});
```

---

## 4. Handler Signatures

Osodio uses `HandlerTraits` to auto-extract typed arguments from `Request`. Any combination of the following parameter types works in any order:

| Type | Resolved from |
|------|--------------|
| `Request&` | The raw request |
| `Response&` | The response builder |
| `PathParam<T, "name">` | URL path segment |
| `Query<T, "name">` | Query string parameter |
| `Query<T, "name", "default">` | Query with default value |
| `Body<T>` | Parsed request body (explicit) |
| `T` (any SCHEMA struct) | Parsed request body (implicit) |
| `Inject<T>` | Service from the DI container |

### Return value auto-serialization

If the handler returns a value (not `void` or `Task<void>`), it is automatically JSON-serialized:

```cpp
// Returns User, automatically serialized as JSON
app.get("/users/:id", [](PathParam<int,"id"> id) -> User {
    return {id.value, "Alice", 30};
});

// Returns nlohmann::json directly
app.get("/info", []() -> nlohmann::json {
    return {{"version","1.0"},{"ok",true}};
});

// Returns nothing — must call res.xxx() explicitly
app.get("/ping", [](Response& res) {
    res.json({{"pong", true}});
});
```

### Async handlers

Add `-> Task<T>` and use `co_await` / `co_return`:

```cpp
app.get("/slow", []() -> Task<nlohmann::json> {
    co_await sleep(500);   // non-blocking 500ms
    co_return nlohmann::json{{"waited_ms", 500}};
});
```

---

## 5. Request API

```cpp
app.get("/echo", [](Request& req, Response& res) {
    req.method;          // "GET", "POST", ...
    req.path;            // "/echo"
    req.version;         // "HTTP/1.1" or "HTTP/2.0"
    req.body;            // raw request body as std::string
    req.remote_ip;       // peer IP, e.g. "192.168.1.42"

    // Headers — lowercase keys
    req.headers["content-type"];
    req.header("Authorization");         // returns std::optional<std::string>
    req.header("X-Custom");              // case-insensitive lookup

    // Path params — populated by the router
    req.params["id"];

    // Query params — e.g. /echo?page=2&q=hello
    req.query["page"];
    req.query_param("page", "1");        // with default value

    // Cancellation
    req.is_cancelled();                  // true if connection was closed
    req.cancel_token;                    // shared_ptr<CancellationToken>

    // Event loop (for manual async work)
    req.loop;                            // core::EventLoop*

    // DI container
    req.container;                       // ServiceContainer*

    // JWT claims (populated by jwt_auth() middleware)
    req.jwt_claims["sub"].get<std::string>();

    res.json({{"ok", true}});
});
```

---

## 6. Response API

All methods return `Response&` for chaining.

```cpp
app.get("/demo", [](Response& res) {
    // Status
    res.status(201);

    // Body types
    res.json({{"key","value"}});         // application/json
    res.text("plain text");              // text/plain
    res.html("<h1>Hello</h1>");          // text/html
    res.send("raw bytes");               // no Content-Type change

    // Headers
    res.header("X-Custom", "value");

    // File download (zero-copy sendfile(2))
    res.header("Content-Type", "application/pdf")
       .send_file("/var/files/doc.pdf");

    // Template rendering (Jinja2 via inja)
    res.render("index.html", {{"user","Alice"},{"items",items}});

    // Redirect (manual)
    res.status(302).header("Location", "/new-path").send("");
});
```

### Chaining example

```cpp
res.status(201)
   .header("X-Request-Id", "abc123")
   .json({{"id", 42}, {"created", true}});
```

---

## 7. Path Parameters

Declare the type and name as template arguments:

```cpp
// int, long, float, double, std::string
app.get("/users/:id",   [](PathParam<int,   "id">   id)   { /* id.value */ });
app.get("/items/:slug", [](PathParam<std::string,"slug"> s){ /* s.value  */ });
app.get("/v/:ver",      [](PathParam<float, "ver">  v)    { /* v.value  */ });
```

**Implicit conversion**: `PathParam<T,N>` converts to `T` automatically:

```cpp
app.get("/users/:id", [](PathParam<int,"id"> id, Inject<DB> db) {
    int user_id = id;   // implicit conversion
    return db->find(user_id);
});
```

---

## 8. Query Parameters

```cpp
// Basic: ?page=2
app.get("/list", [](Query<int,"page"> page) {
    int p = page;          // implicit conversion to int
    bool present = bool(page);  // false if param was absent
});

// With default value: ?page=1&limit=20 (defaults if absent)
app.get("/list", [](Query<int,"page","1">   page,
                    Query<int,"limit","20"> limit,
                    Query<std::string,"q">  q) {
    return nlohmann::json{
        {"page",  (int)page},
        {"limit", (int)limit},
        {"q",     (std::string)q}
    };
});
```

**`operator bool` indicates presence:**
```cpp
app.get("/search", [](Query<std::string,"q"> q, Response& res) {
    if (!q) { res.status(400).json({{"error","q is required"}}); return; }
    // q was present
});
```

---

## 9. Request Body — SCHEMA

### Defining a serializable struct

```cpp
struct CreateUser {
    std::string              name;
    int                      age;
    std::optional<std::string> bio;    // optional: absent/null → std::nullopt

    SCHEMA(CreateUser, name, age, bio)
};
```

**Rules:**
- `SCHEMA(TypeName, field1, field2, ...)` goes inside the struct body
- `std::optional<T>` fields are **automatically optional** — no extra macro
- Non-optional fields missing from the request body → `422 Unprocessable Entity` automatically
- Supports nesting: a field can itself be a SCHEMA struct

### Using the struct as a handler parameter

```cpp
// Implicit (bare struct) — ergonomic
app.post("/users", [](CreateUser body) {
    // body.name, body.age, body.bio
    return nlohmann::json{{"created", body.name}};
});

// Explicit Body<T> — when you need the validity check
app.post("/users", [](Body<CreateUser> body, Response& res) {
    if (!body) return;   // body is invalid (parse/validation failed, res already has 422)
    auto& u = *body;
    return nlohmann::json{{"name", u.name}};
});
```

### Nested structs

```cpp
struct Address {
    std::string street;
    std::string city;
    SCHEMA(Address, street, city)
};

struct CreateProfile {
    std::string name;
    Address     address;
    SCHEMA(CreateProfile, name, address)
};

app.post("/profile", [](CreateProfile p) {
    // p.address.city
});
```

### Returning a struct as JSON

Any struct with `SCHEMA` can be returned directly from a handler — it is auto-serialized:

```cpp
app.get("/users/:id", [](PathParam<int,"id"> id) -> CreateUser {
    return {"Alice", 30, std::nullopt};
});
```

### SCHEMA internals

`SCHEMA(Type, fields...)` generates two friend functions inside the struct:

```cpp
friend void to_json(nlohmann::json& j, const Type& t);
friend void from_json(const nlohmann::json& j, Type& t);
```

The `from_json` uses `osodio::detail::bind_field<FieldType>()`, a template helper that:
- For `std::optional<T>` fields: maps absent/null JSON to `std::nullopt`
- For required fields: calls `j.at(name).get_to(field)`, throwing if absent

**C++26 note**: Once GCC 15+ / Clang with P2996 is available, `SCHEMA` will be eliminated entirely. Any plain struct will serialize automatically via static reflection. Migration: delete the `SCHEMA(...)` line from every struct.

---

## 10. Validation — validate()

Define a `validate()` method returning `std::vector<std::string>`. Return an empty vector for valid data.

```cpp
struct RegisterRequest {
    std::string email;
    std::string password;
    int         age;
    SCHEMA(RegisterRequest, email, age, password)

    std::vector<std::string> validate() const {
        std::vector<std::string> errs;
        if (email.find('@') == std::string::npos)
            errs.push_back("email: invalid format");
        if (password.size() < 8)
            errs.push_back("password: must be at least 8 characters");
        if (age < 18)
            errs.push_back("age: must be at least 18");
        return errs;
    }
};
```

If `validate()` returns a non-empty vector, Osodio automatically responds with:
```json
HTTP 422
{"error": "Validation Failed", "messages": ["email: invalid format", "age: must be at least 18"]}
```

The handler is **not called** if validation fails.

---

## 11. Async Handlers — Task\<T\>

### Making a handler async

Add `-> Task<T>` and use `co_await` / `co_return`:

```cpp
app.get("/async", []() -> Task<nlohmann::json> {
    co_await sleep(100);   // non-blocking, releases the loop thread
    co_return nlohmann::json{{"done", true}};
});
```

### sleep(ms)

Non-blocking sleep. Does not block the event loop thread — other connections continue to be handled during the sleep.

```cpp
app.get("/delayed", [](Request& req) -> Task<nlohmann::json> {
    co_await sleep(2000);   // 2 seconds, non-blocking

    if (req.is_cancelled()) co_return nlohmann::json{};  // client disconnected

    co_return nlohmann::json{{"msg", "hello after 2s"}};
});
```

`sleep(ms)` uses `req.loop` (thread-local) — no need to pass it explicitly.

### Chaining coroutines

```cpp
Task<std::string> fetch_name(int id) {
    co_await sleep(10);
    co_return "Alice";
}

app.get("/users/:id", [](PathParam<int,"id"> id) -> Task<nlohmann::json> {
    auto name = co_await fetch_name(id.value);
    co_return nlohmann::json{{"name", name}};
});
```

### Async middleware

Middleware must use `co_await next()` to continue the chain:

```cpp
app.use([](Request& req, Response& res, NextFn next) -> Task<void> {
    auto start = std::chrono::steady_clock::now();
    co_await next();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();
    res.header("X-Duration-Ms", std::to_string(ms));
});
```

---

## 12. Middleware

### Global middleware

```cpp
app.use(middleware_fn);
```

Middleware runs in registration order for **every** request.

### Middleware signature

```cpp
Middleware = std::function<Task<void>(Request&, Response&, NextFn)>
```

Must `co_await next()` to pass control to the next middleware/handler. Omitting `co_await next()` short-circuits the chain (useful for auth guards).

### Short-circuit example

```cpp
app.use([](Request& req, Response& res, NextFn next) -> Task<void> {
    if (req.header("x-api-key") != "secret") {
        res.status(401).json({{"error","Unauthorized"}});
        co_return;   // do NOT call next()
    }
    co_await next();   // proceed normally
});
```

### Pre/post middleware

```cpp
app.use([](Request& req, Response& res, NextFn next) -> Task<void> {
    // PRE: runs before handler
    req.headers["x-request-id"] = generate_id();

    co_await next();

    // POST: runs after handler completes
    res.header("X-Served-By", "osodio");
});
```

---

## 13. Built-in Middleware

### logger()

Logs every request to stdout: method, path, status, duration.

```cpp
app.use(osodio::logger());
```

Output format: `GET /users 200 1.2ms`

### cors()

Handles CORS preflight and response headers.

```cpp
app.use(osodio::cors());   // allow all origins

app.use(osodio::cors({
    .origins     = {"https://app.example.com", "http://localhost:3000"},
    .methods     = {"GET","POST","PUT","DELETE"},
    .headers     = {"Content-Type","Authorization"},
    .credentials = true,
    .max_age     = 86400,
}));
```

### compress()

Response compression. Prefers Brotli if the client supports it, falls back to gzip.

```cpp
app.use(osodio::compress());

// Custom options
app.use(osodio::compress({
    .min_size  = 1024,        // bytes — don't compress smaller responses
    .brotli    = true,        // enable Brotli (default: true)
    .gzip      = true,        // enable gzip fallback (default: true)
    .level     = 6,           // compression level 1–9
}));
```

### helmet()

Security headers: CSP, HSTS, X-Frame-Options, X-Content-Type-Options, Referrer-Policy.

```cpp
app.use(osodio::helmet());

// Custom CSP
app.use(osodio::helmet({
    .csp        = "default-src 'self'; script-src 'self' cdn.example.com",
    .hsts_max   = 31536000,
    .frame      = "DENY",
}));
```

### rate_limit()

Fixed-window rate limiting by IP (or custom key).

```cpp
app.use(osodio::rate_limit({
    .window_ms = 60'000,   // 1 minute
    .max       = 100,      // 100 requests per window
}));

// Rate limit by API key instead of IP
app.use(osodio::rate_limit({
    .window_ms = 60'000,
    .max       = 1000,
    .key_fn    = [](const Request& req) -> std::string {
        return req.header("x-api-key").value_or(req.remote_ip);
    },
}));
```

Responds with `429 Too Many Requests` when exceeded. Adds headers:
- `X-RateLimit-Limit: 100`
- `X-RateLimit-Remaining: 42`
- `Retry-After: 30`

---

## 14. Route Groups

Groups add a URL prefix and optional per-group middleware.

```cpp
auto api = app.group("/api/v1");

// Per-group middleware — runs only for routes in this group
api.use([](Request&, Response& res, NextFn next) -> Task<void> {
    res.header("X-API-Version", "1");
    co_await next();
});

// Routes under /api/v1/...
api.get("/users",      list_users_handler);
api.post("/users",     create_user_handler);
api.get("/users/:id",  get_user_handler);

// Nested groups
auto admin = api.group("/admin");
admin.use(admin_auth_middleware);
admin.get("/stats", stats_handler);   // → GET /api/v1/admin/stats
```

Groups support all the same methods as `App`: `get`, `post`, `put`, `patch`, `del`, `use`.

---

## 15. Error Handling

### HttpError — throw from any handler

```cpp
#include <osodio/errors.hpp>

app.get("/users/:id", [](PathParam<int,"id"> id, Inject<DB> db) -> User {
    auto user = db->find(id.value);
    if (!user) throw osodio::not_found("User not found");
    return *user;
});
```

**Factory functions:**

```cpp
throw osodio::bad_request("Invalid input");          // 400
throw osodio::unauthorized("Login required");        // 401
throw osodio::forbidden("Access denied");            // 403
throw osodio::not_found("Resource not found");       // 404
throw osodio::method_not_allowed();                  // 405
throw osodio::conflict("Duplicate email");           // 409
throw osodio::unprocessable("Bad data", messages);   // 422
throw osodio::too_many_requests();                   // 429
throw osodio::internal_error("DB failure");          // 500
throw osodio::service_unavailable();                 // 503
```

All produce `{"error": "message"}` JSON bodies.

### on_error() — custom error pages

```cpp
// Specific status code
app.on_error(404, [](int code, Request& req, Response& res) {
    res.json({{"error","Not Found"},{"path",req.path}});
});

// Catch-all for any error status
app.on_error([](int code, Request&, Response& res) {
    res.json({{"error","Something went wrong"},{"code",code}});
});
```

Error handlers run **after** the async handler chain completes. They can inspect and override the response.

---

## 16. Dependency Injection

### Registering services

```cpp
// Singleton: same shared_ptr for every request
app.provide(std::make_shared<Database>("postgres://..."));

// Transient: factory called once per Inject<T> resolution
app.provide<Logger>([]{ return std::make_shared<Logger>(); });
```

### Injecting into handlers

```cpp
app.get("/users", [](Inject<Database> db, Inject<Logger> log) {
    log->info("listing users");
    return db->query("SELECT * FROM users");
});
```

`Inject<T>` has `operator->` and `operator*`:

```cpp
auto rows = db->query("...");   // std::shared_ptr-like
auto& ref = *db;
```

### Multiple services

```cpp
struct Config { std::string base_url; };
struct Cache  { /* ... */ };

app.provide(std::make_shared<Config>(Config{"https://api.example.com"}));
app.provide(std::make_shared<Cache>());

app.get("/data", [](Inject<Config> cfg, Inject<Cache> cache) {
    // ...
});
```

---

## 17. JWT Authentication

### Signing a token

```cpp
#include <osodio/jwt.hpp>

// HS256 (shared secret)
auto token = osodio::jwt::sign(
    {{"sub", "user-42"}, {"exp", osodio::jwt::expires_in(3600)}},
    "my-secret"
);

// RS256 (RSA private key PEM)
auto token = osodio::jwt::sign(
    {{"sub", "user-42"}},
    private_key_pem,
    "RS256"
);
```

### jwt_auth() middleware

```cpp
// HS256 — all routes
app.use(osodio::jwt_auth("my-secret"));

// With options
app.use(osodio::jwt_auth("my-secret", {
    .issuer   = "my-app",
    .audience = "api",
    .skip = [](const Request& req) {
        return req.path == "/login" || req.path == "/health";
    }
}));

// RS256 — convenience overload
app.use(osodio::jwt_auth_rsa(public_key_pem));
```

After successful validation, claims are in `req.jwt_claims`:

```cpp
app.get("/me", [](Request& req, Response& res) {
    auto sub = req.jwt_claims["sub"].get<std::string>();
    res.json({{"user_id", sub}});
});
```

### Manual verify

```cpp
try {
    auto claims = osodio::jwt::verify(token, "my-secret");
    auto sub = claims["sub"].get<std::string>();
} catch (const osodio::JwtError& e) {
    // "token expired", "invalid signature", etc.
}
```

### jwt::decode (no verification)

```cpp
// Inspect without verifying — useful for debugging
auto claims = osodio::jwt::decode(token);
```

### JwtOptions fields

```cpp
struct JwtOptions {
    std::string algorithm  = "HS256";        // "HS256" or "RS256"
    std::string header     = "authorization"; // which header to read
    bool        bearer     = true;           // expect "Bearer <token>"
    std::optional<std::string> issuer;       // validate "iss" claim
    std::optional<std::string> audience;     // validate "aud" claim
    bool check_exp = true;                   // reject expired tokens
    bool check_nbf = true;                   // reject not-yet-valid tokens
};
```

---

## 18. Server-Sent Events (SSE)

### HTTP/1.1

```cpp
#include <osodio/sse.hpp>

app.get("/events", [](Request& req, Response& res) -> Task<void> {
    auto sse = osodio::make_sse(res, req);

    int n = 0;
    while (sse.is_open()) {
        sse.send("tick", std::to_string(n++));   // named event
        co_await sleep(1000);
    }
});
```

### SSEWriter API

```cpp
auto sse = osodio::make_sse(res, req);

sse.send("data only");                         // data: data only\n\n
sse.send("update", R"({"count":42})");         // event: update\ndata: ...\n\n
sse.send("update", data, "event-id-123");      // with id
sse.ping();                                    // : ping\n\n (keepalive)
sse.is_open();                                 // false when client disconnects
```

### HTTP/2 SSE

Works automatically. When the request comes over HTTP/2, `make_sse` uses nghttp2 DATA frames instead of raw socket writes. The handler code is **identical**:

```cpp
app.get("/events", [](Request& req, Response& res) -> Task<void> {
    auto sse = osodio::make_sse(res, req);  // same for HTTP/1.1 and HTTP/2
    while (sse.is_open()) {
        sse.send("message", "hello");
        co_await sleep(500);
    }
});
```

---

## 19. WebSockets

### HTTP/1.1 (RFC 6455)

```cpp
#include <osodio/websocket.hpp>

app.ws("/chat", [](WSConnection ws) -> Task<void> {
    while (ws.is_open()) {
        auto msg = co_await ws.recv();
        if (!msg) break;                              // connection closed

        if (msg->type == WSMessageType::Text) {
            ws.send("echo: " + msg->data);
        } else if (msg->type == WSMessageType::Binary) {
            ws.send_binary(msg->data);
        }
    }
});
```

### WSConnection API

```cpp
// Receiving
auto msg = co_await ws.recv();       // returns std::optional<WSMessage>
if (!msg) break;                     // nullopt = connection closed
msg->type;                           // WSMessageType::Text | Binary | Ping | Pong | Close
msg->data;                           // std::string payload

// Sending
ws.send("text message");             // UTF-8 text frame
ws.send_binary(bytes);               // binary frame
ws.close(1000, "bye");               // send close frame
ws.is_open();                        // false after close handshake
```

### HTTP/2 WebSocket (RFC 8441)

The framework handles the protocol difference transparently. Register the handler with `app.ws()` — it works for both HTTP/1.1 (101 Upgrade) and HTTP/2 (CONNECT + `:protocol: websocket`):

```cpp
app.ws("/ws", [](WSConnection ws) -> Task<void> {
    // Identical code for both HTTP/1.1 and HTTP/2
    while (ws.is_open()) {
        auto msg = co_await ws.recv();
        if (!msg) break;
        ws.send("got: " + msg->data);
    }
});
```

### Broadcast pattern

```cpp
struct ChatRoom {
    std::mutex mtx;
    std::vector<std::function<void(std::string)>> subscribers;
    // ...
};

app.ws("/room", [](WSConnection ws, Inject<ChatRoom> room) -> Task<void> {
    auto id = room->subscribe([&ws](std::string msg){ ws.send(msg); });
    while (ws.is_open()) {
        auto msg = co_await ws.recv();
        if (!msg) break;
        room->broadcast(msg->data);
    }
    room->unsubscribe(id);
});
```

---

## 20. Static Files & SPA

### Static file serving

```cpp
// Serve ./public under /static
// GET /static/app.js → ./public/app.js
app.serve_static("/static", "./public");
```

Features: MIME types, ETag, Cache-Control, 304 Not Modified, sendfile(2) zero-copy, path traversal protection.

### SPA fallback

Any path not matching a real file is served as `index.html`:

```cpp
// Enable SPA mode (React/Vue/Svelte)
app.serve_static("/", "./dist", true);
```

This allows client-side routing: `/app/dashboard`, `/app/settings`, etc. all return `index.html` with `200 OK`.

---

## 21. HTML Templates

Osodio uses [inja](https://github.com/pantor/inja), a Jinja2-compatible template engine.

### Setup

```cpp
app.set_templates("./templates");
```

### Rendering

```cpp
app.get("/hello", [](Response& res) {
    res.render("hello.html", {
        {"name", "Alice"},
        {"items", nlohmann::json::array({"one","two","three"})}
    });
});
```

### Template syntax (Jinja2)

```html
<!-- templates/hello.html -->
<h1>Hello, {{ name }}!</h1>
<ul>
{% for item in items %}
  <li>{{ item }}</li>
{% endfor %}
</ul>
```

Templates are parsed once and cached per thread.

---

## 22. OpenAPI / Swagger UI

### Enable

```cpp
app.api_info("My API", "1.0.0");   // title and version
app.enable_docs();                  // /openapi.json + /docs

// Custom paths
app.enable_docs("/api/spec.json", "/api/ui");
```

Route documentation is captured **at compile time** via `HandlerTraits` — no annotations needed. The spec is generated when `run()` or `prepare()` is called.

### What gets captured

- HTTP method and path
- Path parameter names and types
- Query parameter names, types, and defaults
- Request body schema (from SCHEMA structs)
- Response type (from return type)

### Example

```cpp
app.enable_docs();

app.get("/users/:id", [](PathParam<int,"id"> id) -> User {
    // ...
});
// → Swagger UI at /docs shows GET /users/{id} with int path param
```

---

## 23. Health & Metrics

```cpp
app.enable_health();    // GET /health  → JSON status
app.enable_metrics();   // GET /metrics → Prometheus text

// Custom paths
app.enable_health("/healthz");
app.enable_metrics("/prometheus");
```

### /health response

```json
{
  "status": "ok",
  "uptime_seconds": 3600,
  "active_connections": 42,
  "total_requests": 15000
}
```

### /metrics response (Prometheus)

```
# HELP osodio_requests_total Total requests handled
# TYPE osodio_requests_total counter
osodio_requests_total 15000

# HELP osodio_active_connections Currently active connections
# TYPE osodio_active_connections gauge
osodio_active_connections 42

# HELP osodio_uptime_seconds Server uptime in seconds
# TYPE osodio_uptime_seconds gauge
osodio_uptime_seconds 3600
```

---

## 24. TLS / HTTPS

```cpp
app.tls("server.crt", "server.key").run(443);
```

Both files must be PEM format. Osodio:
- Requires TLS 1.2+ (TLS 1.3 preferred)
- Enables ALPN (`h2`/`http/1.1`) automatically — HTTP/2 is negotiated when available
- Handles the TLS handshake asynchronously (non-blocking)
- Uses `sendfile(2)` fallback via `read()` for TLS (kernel zero-copy unavailable over TLS)

### Self-signed cert for development

```bash
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem \
  -days 365 -nodes -subj "/CN=localhost"
```

```cpp
app.tls("cert.pem", "key.pem").run(8443);
```

---

## 25. HTTP/2

HTTP/2 is enabled automatically when TLS is active and the client supports it (ALPN negotiation). No code changes required.

### What works over HTTP/2

| Feature | HTTP/1.1 | HTTP/2 |
|---------|----------|--------|
| Regular requests | ✓ | ✓ |
| SSE | ✓ | ✓ (DATA frames) |
| WebSocket | ✓ (101) | ✓ (RFC 8441 CONNECT) |
| TLS required | No | Yes (via ALPN) |

### HTTP/2 push is not implemented

Server push was removed from the roadmap — browser support was dropped.

---

## 26. Multipart Uploads

```cpp
#include <osodio/multipart.hpp>

app.post("/upload", [](Request& req, Response& res) {
    auto parts = osodio::parse_multipart(req);

    for (const auto& part : parts) {
        part.name;            // form field name
        part.filename;        // original filename (if file upload)
        part.content_type;    // e.g. "image/jpeg"
        part.body;            // raw file/field content as std::string
        part.headers;         // all part headers

        if (!part.filename.empty()) {
            // Save the file
            std::ofstream f("uploads/" + part.filename, std::ios::binary);
            f.write(part.body.data(), part.body.size());
        }
    }

    res.json({{"uploaded", parts.size()}});
});
```

`parse_multipart` returns an empty vector if the Content-Type is not `multipart/form-data` or the boundary is malformed.

---

## 27. TestClient — In-Process Testing

`TestClient` executes requests directly against the App without a network socket. The full middleware + router pipeline runs synchronously in the calling thread.

```cpp
#include <osodio/testing.hpp>

App app;
app.get("/hello", [](Response& res) { res.json({{"msg","hi"}}); });

osodio::TestClient client(app);
auto r = client.get("/hello").send();

assert(r.status == 200);
assert(r.ok());                          // true for 2xx
assert(r.json()["msg"] == "hi");
assert(r.header("Content-Type").find("json") != std::string::npos);
```

### Request builder API

```cpp
// Headers
client.get("/protected")
      .header("Authorization", "Bearer " + token)
      .send();

// JSON body
client.post("/users")
      .json({{"name","Alice"},{"age",30}})
      .send();

// Raw body
client.post("/data")
      .body("raw bytes", "application/octet-stream")
      .send();

// Query params
client.get("/search")
      .query("q", "hello")
      .query("page", "2")
      .send();

// Inline query string
client.get("/search?q=hello&page=2").send();
```

### TestClient::Response fields

```cpp
r.status;            // int, e.g. 200
r.ok();              // true for 2xx
r.body;              // std::string raw body
r.json();            // parse body as nlohmann::json (throws on invalid JSON)
r.header("X-Key");   // std::string, "" if absent
r.headers;           // std::unordered_map<std::string,std::string>
```

### Testing with middleware

```cpp
App app;
app.use(osodio::jwt_auth("secret", {.skip = [](auto& r){ return r.path=="/login"; }}));
app.get("/me", [](Request& req, Response& res) {
    res.json({{"sub", req.jwt_claims["sub"]}});
});

TestClient client(app);

// Without token → 401
auto r1 = client.get("/me").send();
assert(r1.status == 401);

// With valid token → 200
auto token = jwt::sign({{"sub","42"}}, "secret");
auto r2 = client.get("/me")
               .header("Authorization", "Bearer " + token)
               .send();
assert(r2.status == 200);
assert(r2.json()["sub"] == "42");
```

### Testing body validation

```cpp
struct CreateUser {
    std::string name;
    int age;
    SCHEMA(CreateUser, name, age)
    std::vector<std::string> validate() const {
        if (age < 18) return {"age: must be at least 18"};
        return {};
    }
};

app.post("/users", [](CreateUser u, Response& res) {
    res.status(201).json({{"name", u.name}});
});

TestClient client(app);

// Missing field → 422
auto r1 = client.post("/users").json({{"name","Bob"}}).send();
assert(r1.status == 422);

// Business rule violation → 422
auto r2 = client.post("/users").json({{"name","Bob"},{"age",15}}).send();
assert(r2.status == 422);
assert(r2.json()["messages"][0] == "age: must be at least 18");

// Valid → 201
auto r3 = client.post("/users").json({{"name","Bob"},{"age",25}}).send();
assert(r3.status == 201);
```

### Notes

- `sleep()` calls return immediately in TestClient (no event loop — intentional)
- SSE and WebSocket handlers cannot be tested via TestClient (throws `std::logic_error`)
- `send_file()` responses: the file is read into `body` so tests can inspect content
- Call `TestClient client(app)` **after** all routes are registered

---

## 28. Graceful Shutdown

Osodio handles `SIGINT` and `SIGTERM`:

1. Stops accepting new connections
2. Waits up to 30 seconds for active connections to finish
3. Prints status and exits cleanly

A second `SIGINT` forces immediate exit.

```
CTRL+C
Shutting down gracefully... (CTRL+C again to force)
All connections drained.
```

No configuration needed — works automatically.

---

## 29. CancellationToken

Each request has a `CancellationToken` shared between the connection and the handler. It is cancelled when:
- The client disconnects
- A timeout fires (5s header timeout, 30s handler timeout)
- A write error occurs

### Checking cancellation

```cpp
app.get("/stream", [](Request& req) -> Task<nlohmann::json> {
    for (int i = 0; i < 100; ++i) {
        if (req.is_cancelled()) co_return nlohmann::json{};

        co_await sleep(100);
        // do work...
    }
    co_return nlohmann::json{{"done", true}};
});
```

`sleep()` automatically respects the token — if the connection closes during a sleep, the coroutine is resumed immediately rather than waiting for the full duration.

---

## 30. Architecture Internals

### Event loop

```
EventLoop (epoll + timerfd + eventfd)
  ├── add/modify/remove fd → callback map
  ├── post(fn) → queued task (thread-safe via eventfd wakeup)
  └── schedule_timer(ms, fn) → timerfd one-shot
```

Multi-threaded: `hardware_concurrency()` threads, each with its own `EventLoop` + `TcpServer`. `SO_REUSEPORT` distributes connections.

### Request lifetime

```
accept() → HttpConnection created
  → epoll EPOLLIN fires → HttpParser feeds bytes → ParsedRequest ready
  → DispatchFn(req, res) coroutine started
  → middleware chain runs
  → router.match() → HandlerTraits::call()
  → co_await extracts typed args, calls user lambda
  → result auto-serialized if non-void
  → response bytes written to socket (EPOLLOUT backpressure if needed)
  → connection closed or keep-alive loop restarts
```

### io_uring backend (optional)

Build with `-DUSE_IO_URING=ON` to use the io_uring backend instead of epoll. Same interface, lower syscall overhead:

```cmake
cmake -B build -DUSE_IO_URING=ON
```

Requires Linux 5.1+. The backend uses `IORING_POLL_ADD_MULTI` (persistent multishot poll) so a single `io_uring_enter` serves all file descriptors.

### File structure

```
include/osodio/
  osodio.hpp        — single include
  app.hpp           — App, routing, run(), prepare(), handle_request()
  request.hpp       — Request struct
  response.hpp      — Response builder
  router.hpp        — Radix tree (STATIC/PARAM/WILDCARD nodes)
  types.hpp         — Middleware, Handler, DispatchFn, NextFn, ErrorHandler
  task.hpp          — Task<T>, SleepAwaitable, current_loop/current_token
  cancel.hpp        — CancellationToken with early-wake set_wake()
  handler_traits.hpp — HandlerTraits, extractor<T>, extract_body
  schema.hpp        — SCHEMA macro, bind_field, is_optional_v
  validation.hpp    — has_validate trait, ValidationError
  errors.hpp        — HttpError, not_found(), bad_request(), etc.
  di.hpp            — ServiceContainer, Inject<T>
  middleware.hpp    — logger(), cors(), compress(), helmet(), rate_limit()
  jwt.hpp           — jwt::sign/verify/decode, jwt_auth(), jwt_auth_rsa()
  sse.hpp           — SSEWriter, make_sse()
  websocket.hpp     — WSConnection, WSMessage, WSState, frame builder
  multipart.hpp     — MultipartPart, parse_multipart()
  openapi.hpp       — DocBuilder<F>, build_openapi_doc(), swagger_ui_html()
  metrics.hpp       — Metrics singleton, Prometheus + JSON
  group.hpp         — RouteGroup
  testing.hpp       — TestClient, RequestBuilder, TestClient::Response
  core/
    event_loop.hpp  — EpollLoop (alias: EventLoop), IoUringLoop

src/
  app.cpp               — App::run(), prepare(), handle_request(), static files
  router.cpp            — Radix tree implementation
  core/
    event_loop.cpp      — epoll, timerfd, eventfd
    tcp_server.cpp      — accept loop, connection limit
    io_uring_loop.cpp   — io_uring backend (compiled if USE_IO_URING=ON)
  http/
    http_parser.hpp/cpp    — llhttp wrapper
    http_connection.hpp/cpp — per-connection state, timeouts, write buffer
    http2_connection.hpp/cpp — HTTP/2 via nghttp2, streams, SSE, WS

third_party/          — vendored, zero network in cmake
  nlohmann/json.hpp   — v3.11.3
  simdjson.h/.cpp     — v3.10.0
  inja.hpp            — v3.4.0
  llhttp/             — v9.2.1
```

### Key design choices

**Why epoll, not io_uring by default?** epoll works on all Linux kernels ≥ 2.6. io_uring requires 5.1+ and its API is more complex. io_uring is available as an opt-in backend with `-DUSE_IO_URING=ON`.

**Why not Boost.Asio?** 200+ MB of headers, complex cancellation model. Osodio's event loop is ~300 lines of readable C++.

**Why `fixed_string` as NTTP?** C++20 allows string literals as template parameters. `PathParam<int,"id">` is more readable than a tag-type pattern and requires no boilerplate.

**Why SCHEMA inside the struct?** `NLOHMANN_DEFINE_TYPE_INTRUSIVE` generates `friend` functions that access private members. Keeping it inside the struct makes the type self-contained.

**Why symmetric transfer in Task<T>?** Direct coroutine-to-coroutine resumption via `std::coroutine_handle<>` avoids stack growth when chaining middleware (each `co_await next()` is a tail-call at the coroutine level).

**Write buffer backpressure:** each connection has a 16MB write buffer cap. When the socket buffer is full, EPOLLOUT is registered and writing resumes when the kernel has space. If the cap is exceeded, the connection is closed cleanly.

---

## Appendix: Common Patterns

### JSON API with auth and validation

```cpp
struct LoginRequest {
    std::string email;
    std::string password;
    SCHEMA(LoginRequest, email, password)
};

struct UserProfile {
    int         id;
    std::string email;
    std::string name;
    SCHEMA(UserProfile, id, email, name)
};

int main() {
    App app;

    app.use(osodio::logger());
    app.use(osodio::cors());

    // Public routes
    app.post("/login", [](LoginRequest body, Inject<AuthService> auth) -> nlohmann::json {
        auto user = auth->check(body.email, body.password);
        if (!user) throw osodio::unauthorized("Invalid credentials");
        auto token = osodio::jwt::sign(
            {{"sub", user->id}, {"exp", osodio::jwt::expires_in(86400)}},
            auth->secret()
        );
        return {{"token", token}};
    });

    // Protected routes
    auto api = app.group("/api");
    api.use(osodio::jwt_auth("my-secret", {
        .issuer = "my-app"
    }));

    api.get("/me", [](Request& req, Inject<UserService> users) -> UserProfile {
        auto sub = req.jwt_claims["sub"].get<int>();
        auto user = users->find(sub);
        if (!user) throw osodio::not_found();
        return *user;
    });

    app.run(8080);
}
```

### Real-time with SSE

```cpp
struct EventBus {
    std::mutex mtx;
    std::vector<std::function<void(std::string)>> listeners;

    void subscribe(std::function<void(std::string)> fn) {
        std::lock_guard g(mtx);
        listeners.push_back(std::move(fn));
    }
    void emit(const std::string& event) {
        std::lock_guard g(mtx);
        for (auto& fn : listeners) fn(event);
    }
};

app.provide(std::make_shared<EventBus>());

app.get("/events", [](Request& req, Response& res,
                       Inject<EventBus> bus) -> Task<void> {
    auto sse = osodio::make_sse(res, req);
    bus->subscribe([&sse](std::string ev){ sse.send("update", ev); });
    while (sse.is_open()) co_await sleep(5000);   // keepalive
});

app.post("/trigger", [](Inject<EventBus> bus, Response& res) {
    bus->emit(R"({"event":"something happened"})");
    res.status(204).send("");
});
```
