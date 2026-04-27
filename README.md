# Osodio

C++20 async HTTP framework. epoll event loop, coroutines, zero-boilerplate body parsing, JWT, SSE, WebSockets — all in a single header include.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

struct CreatePost {
    std::string title;
    std::string content;
    std::optional<std::string> tags;   // absent or null in JSON → std::nullopt
    SCHEMA(CreatePost, title, content, tags)

    std::vector<std::string> validate() const {
        if (title.size() < 3) return {"title: min 3 characters"};
        if (content.empty())  return {"content: required"};
        return {};
    }
};

int main() {
    App app;
    app.use(osodio::logger());
    app.use(osodio::cors({.origins = {"https://myapp.com"}}));

    auto api = app.group("/api/v1");
    api.use(osodio::jwt_auth("my-secret"));

    // Body auto-parsed + validated. 422 on failure, no wrapper needed.
    api.post("/posts", [](CreatePost post, Request& req) -> nlohmann::json {
        std::string author = req.jwt_claims.value("sub", "");
        return {{"id", 1}, {"title", post.title}, {"author", author}};
    });

    api.get("/posts/:id", [](PathParam<int,"id"> id) -> Task<nlohmann::json> {
        co_await sleep(0);   // async, non-blocking
        co_return nlohmann::json{{"id", (int)id}, {"title", "Hello world"}};
    });

    app.run(8080);
}
```

---

## Features

| | |
|---|---|
| **Routing** | Radix tree, `:param` / `{param}` styles, `*` wildcards |
| **Body parsing** | Any `SCHEMA` struct as a bare parameter — no wrapper needed |
| **Optional fields** | `std::optional<T>` fields: absent or null in body → `std::nullopt` |
| **Validation** | Define `validate()` → `vector<string>` inside any schema struct; 422 on failure |
| **Handler injection** | `PathParam`, `Query`, body structs, `Inject<T>`, `Request&`, `Response&` — all auto-extracted |
| **Async** | C++20 `Task<T>` coroutines, `co_await sleep(ms)`, full coroutine chaining |
| **Cancellation** | `req.is_cancelled()`, `CancellationToken`; `sleep()` exits early on disconnect |
| **Route groups** | `app.group("/prefix").use(mw)` — URL prefix + per-group middleware |
| **Middleware** | `logger()`, `cors()`, `compress()`, `helmet()`, `rate_limit()`, `jwt_auth()` built-in |
| **JWT** | `jwt::sign` / `jwt::verify` / `jwt_auth()` — HS256 and RS256, claim validation |
| **Dependency injection** | `app.provide<T>(...)` / `Inject<T>` in any handler — singleton and transient |
| **Static files** | `serve_static(prefix, dir, spa)` — MIME, ETag, Cache-Control, 304, `sendfile(2)`, SPA fallback |
| **SSE** | `make_sse(res, req)` — `text/event-stream`, named events, keepalive pings, auto-disconnect |
| **WebSockets** | `app.ws(path, handler)` — RFC 6455, binary/text frames, ping/pong, fragmentation |
| **Multipart** | `parse_multipart(req)` — file uploads, field names, Content-Type per part |
| **Templates** | `res.render("page.html", data)` via inja (Jinja2-compatible) |
| **OpenAPI + Swagger** | `/openapi.json` and `/docs` auto-generated from handler signatures at startup |
| **Compression** | `compress()` — gzip + Brotli, negotiated by `Accept-Encoding` |
| **Rate limiting** | Fixed-window per IP or custom key; `X-RateLimit-*` headers |
| **Security headers** | `helmet()` — CSP, HSTS, X-Frame-Options, X-Content-Type-Options |
| **HTTP/1.1** | Keep-alive, llhttp parser, non-blocking writes, `sendfile(2)` for statics |
| **HTTPS / HTTP/2** | TLS via OpenSSL, HTTP/2 via nghttp2 with ALPN negotiation |
| **Multi-core** | One epoll loop per hardware thread, SO_REUSEPORT, shared connection limit |
| **Graceful shutdown** | SIGTERM drains active connections (30s grace), second signal forces exit |
| **Vendored deps** | 8 files in `third_party/` — no network during cmake |

---

## Examples

### REST API with JWT auth

A complete login → token → protected CRUD flow.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

// ── Models ────────────────────────────────────────────────────────────────────

struct LoginRequest {
    std::string username;
    std::string password;
    SCHEMA(LoginRequest, username, password)
};

struct CreateArticle {
    std::string  title;
    std::string  body;
    std::optional<std::vector<std::string>> tags;
    SCHEMA(CreateArticle, title, body, tags)

    std::vector<std::string> validate() const {
        std::vector<std::string> errs;
        if (title.size() < 5)  errs.push_back("title: min 5 characters");
        if (body.size() < 20)  errs.push_back("body: min 20 characters");
        return errs;
    }
};

struct Article {
    int         id;
    std::string title;
    std::string body;
    std::string author;
};

struct DB {
    std::vector<Article> articles;
    int next_id = 1;
};

// ── App ───────────────────────────────────────────────────────────────────────

const std::string JWT_SECRET = "change-this-in-production";

int main() {
    App app;
    app.provide(std::make_shared<DB>());
    app.api_info("Articles API", "1.0.0");

    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::cors({
        .origins     = {"https://myapp.com", "http://localhost:5173"},
        .credentials = true,
    }));
    app.use(osodio::helmet());
    app.use(osodio::rate_limit({.requests = 200, .window_seconds = 60}));

    // ── Public: login ─────────────────────────────────────────────────────────

    app.post("/auth/login", [](LoginRequest req) -> nlohmann::json {
        // In production: verify against a hashed password in the DB.
        if (req.username != "alice" || req.password != "hunter2")
            throw osodio::unauthorized("invalid credentials");

        auto token = jwt::sign({
            {"sub",  req.username},
            {"role", "editor"},
            {"exp",  jwt::expires_in(86400)},  // 24 h
        }, JWT_SECRET);

        return {{"token", token}, {"expires_in", 86400}};
    });

    // ── Protected routes ──────────────────────────────────────────────────────

    auto api = app.group("/api/v1");
    api.use(osodio::jwt_auth(JWT_SECRET, {
        // Allow unauthenticated reads
        .skip = [](const Request& req) { return req.method == "GET"; },
    }));

    api.get("/articles", [](Inject<DB> db,
                             Query<int,"page","1">    page,
                             Query<int,"limit","20">  limit) -> nlohmann::json {
        int p = std::max(1, (int)page);
        int l = std::clamp((int)limit, 1, 100);
        int start = (p - 1) * l;

        nlohmann::json list = nlohmann::json::array();
        for (size_t i = start;
             i < db->articles.size() && (int)(i - start) < l; ++i) {
            list.push_back({
                {"id",     db->articles[i].id},
                {"title",  db->articles[i].title},
                {"author", db->articles[i].author},
            });
        }
        return {{"articles", list}, {"page", p}, {"total", db->articles.size()}};
    });

    api.get("/articles/:id", [](PathParam<int,"id"> id, Inject<DB> db) -> nlohmann::json {
        for (auto& a : db->articles)
            if (a.id == id.value)
                return {{"id",a.id},{"title",a.title},{"body",a.body},{"author",a.author}};
        throw osodio::not_found("article not found");
    });

    // jwt_claims populated by jwt_auth(); contains "sub", "role", "exp".
    api.post("/articles", [](CreateArticle req, Request& r, Inject<DB> db) -> nlohmann::json {
        Article a{db->next_id++, req.title, req.body, r.jwt_claims.value("sub","")};
        db->articles.push_back(a);
        return {{"id", a.id}, {"title", a.title}};
    });

    api.del("/articles/:id", [](PathParam<int,"id"> id, Request& r, Inject<DB> db) {
        if (r.jwt_claims.value("role","") != "editor")
            throw osodio::forbidden("editor role required");
        auto it = std::find_if(db->articles.begin(), db->articles.end(),
                               [&](auto& a){ return a.id == id.value; });
        if (it == db->articles.end())
            throw osodio::not_found("article not found");
        db->articles.erase(it);
        return nlohmann::json{{"ok", true}};
    });

    // ── Error handlers ────────────────────────────────────────────────────────

    app.on_error(404, [](int, Request& req, Response& res) {
        res.json({{"error", "Not Found"}, {"path", req.path}});
    });
    app.on_error([](int code, Request&, Response& res) {
        res.json({{"error", "Something went wrong"}, {"code", code}});
    });

    app.enable_docs();   // GET /docs + GET /openapi.json
    app.run(8080);
}
```

---

### File uploads

Accept a multipart form, validate the file type, and save to disk.

```cpp
#include <osodio/osodio.hpp>
#include <fstream>
#include <filesystem>
using namespace osodio;

int main() {
    App app;
    app.use(osodio::logger());

    std::filesystem::create_directories("uploads");

    // POST /upload/avatar   (multipart/form-data, field name "file")
    app.post("/upload/avatar", [](Request& req, Response& res) -> Task<void> {
        auto parts = osodio::parse_multipart(req);
        if (!parts) {
            res.status(400).json({{"error","expected multipart/form-data"}});
            co_return;
        }

        for (auto& part : *parts) {
            if (part.name != "file") continue;

            if (part.filename.empty()) {
                res.status(400).json({{"error","field 'file' has no filename"}}); co_return;
            }
            if (part.content_type.rfind("image/", 0) != 0) {
                res.status(415).json({{"error","only image/* accepted"}}); co_return;
            }
            if (part.body.size() > 5 * 1024 * 1024) {
                res.status(413).json({{"error","max 5 MB"}}); co_return;
            }

            // Sanitise the filename — never trust user-supplied paths.
            auto safe = std::filesystem::path(part.filename).filename().string();
            std::ofstream f("uploads/" + safe, std::ios::binary);
            f.write(part.body.data(), static_cast<std::streamsize>(part.body.size()));

            res.status(201).json({
                {"url",  "/static/uploads/" + safe},
                {"size", part.body.size()},
                {"type", part.content_type},
            });
            co_return;
        }

        res.status(400).json({{"error","no field named 'file' found"}});
    });

    // Serve uploaded files back
    app.serve_static("/static/uploads", "./uploads");

    app.run(8080);
}
```

---

### Live updates with Server-Sent Events

A dashboard that receives real-time metrics from the server. The browser
reconnects automatically using `Last-Event-ID` if the connection drops.

```cpp
#include <osodio/osodio.hpp>
#include <ctime>
using namespace osodio;

struct Metrics {
    std::atomic<int> requests{0};
    std::atomic<int> errors{0};
};

int main() {
    App app;
    app.provide(std::make_shared<Metrics>());

    // Every request increments the counter
    app.use([](Request&, Response& res, auto next) -> Task<void> {
        co_await next();
        // (metrics would be injected in a real app)
    });

    app.use(osodio::cors());

    // SSE stream — one long-lived GET, no polling needed from the client.
    app.get("/metrics/live", [](Request& req, Response& res,
                                Inject<Metrics> m) -> Task<void> {
        auto sse = osodio::make_sse(res, req);

        // Send current state immediately so the client has data on connect.
        sse.send_event("snapshot", nlohmann::json{
            {"requests", m->requests.load()},
            {"errors",   m->errors.load()},
            {"ts",       std::time(nullptr)},
        }.dump(), std::to_string(std::time(nullptr)));

        int tick = 0;
        while (sse.is_open()) {
            co_await osodio::sleep(2000);
            if (req.is_cancelled()) break;

            // Push a delta every 2 s
            sse.send_event("delta", nlohmann::json{
                {"requests", m->requests.load()},
                {"errors",   m->errors.load()},
                {"ts",       std::time(nullptr)},
            }.dump(), std::to_string(++tick));

            // Keepalive comment every 10 ticks so proxies don't close the stream
            if (tick % 10 == 0) sse.ping("keepalive");
        }
    });

    // Dashboard HTML
    app.get("/dashboard", [](Response& res) {
        res.html(R"html(<!DOCTYPE html>
<html><body>
<h1>Live metrics</h1>
<pre id="out"></pre>
<script>
  const es = new EventSource("/metrics/live");
  es.addEventListener("snapshot", e => out.textContent = e.data);
  es.addEventListener("delta",    e => out.textContent = e.data);
</script>
</body></html>)html");
    });

    app.run(8080);
}
```

---

### WebSocket: collaborative counter

Multiple browser tabs share a counter. Any tab can increment or reset; all
connected clients see the change immediately.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

// In production: use a DI service and proper locking.
std::atomic<int> g_counter{0};

int main() {
    App app;
    app.use(osodio::cors());

    app.ws("/counter", [](WSConnection ws) -> Task<void> {
        // New connection: send the current value right away.
        ws.send(nlohmann::json{{"count", g_counter.load()}}.dump());

        while (ws.is_open()) {
            auto msg = co_await ws.recv();
            if (!msg || msg->is_close()) break;
            if (!msg->is_text()) continue;

            auto data = nlohmann::json::parse(msg->data, nullptr, /*exceptions=*/false);
            if (data.is_discarded()) continue;

            std::string action = data.value("action", "");
            if (action == "increment") {
                ws.send(nlohmann::json{{"count", ++g_counter}}.dump());
            } else if (action == "decrement") {
                ws.send(nlohmann::json{{"count", --g_counter}}.dump());
            } else if (action == "reset") {
                g_counter = 0;
                ws.send(nlohmann::json{{"count", 0}}.dump());
            }
        }
    });

    app.get("/", [](Response& res) {
        res.html(R"html(<!DOCTYPE html>
<html><body>
<h1 id="count">...</h1>
<button onclick="ws.send(JSON.stringify({action:'increment'}))">+</button>
<button onclick="ws.send(JSON.stringify({action:'decrement'}))">-</button>
<button onclick="ws.send(JSON.stringify({action:'reset'}))">reset</button>
<script>
  const ws = new WebSocket("ws://localhost:8080/counter");
  ws.onmessage = e => count.textContent = JSON.parse(e.data).count;
</script>
</body></html>)html");
    });

    app.run(8080);
}
```

---

### Deploying a React / Vue SPA

Serve the compiled frontend and let the client-side router handle all paths.
The API lives under `/api`; everything else serves `index.html`.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

int main() {
    App app;

    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::helmet());
    app.use(osodio::cors({.origins = {"https://myapp.com"}}));

    // API routes
    auto api = app.group("/api/v1");
    api.use(osodio::jwt_auth("secret"));
    api.get("/me", [](Request& req) -> nlohmann::json {
        return {{"sub", req.jwt_claims.value("sub", "")},
                {"role", req.jwt_claims.value("role", "")}};
    });
    // ... more routes

    // SPA fallback: any path not matched above → serve ./dist/index.html
    // Hashed assets (app.abc123.js) get Cache-Control: immutable for 1 year.
    // All other files get Cache-Control: max-age=3600, must-revalidate.
    app.serve_static("/", "./dist", /*spa=*/true);

    app.run(8080);
}
```

---

## API Reference

### App

```cpp
App app;
app.run(8080);                        // 0.0.0.0:8080
app.run("127.0.0.1", 3000);
app.run();                            // 0.0.0.0:5000

app.api_info("My API", "1.0.0");     // shown in /docs and /openapi.json
app.enable_docs();                    // GET /openapi.json + GET /docs
app.enable_docs("/api.json", "/ui");  // custom paths
app.enable_health();                  // GET /health → JSON status
app.enable_metrics();                 // GET /metrics → Prometheus text

app.max_connections(10'000);          // 503 beyond this limit (default: 10 000)
app.set_templates("./views");         // template root (default: ./templates)
```

### Routing

```cpp
app.get   ("/path", handler);
app.post  ("/path", handler);
app.put   ("/path", handler);
app.patch ("/path", handler);
app.del   ("/path", handler);
app.any   ("/path", handler);    // matches all HTTP methods
```

Patterns: `/users/:id` · `/users/{id}` · `/files/*`

### Route Groups

```cpp
auto api = app.group("/api/v1");
api.use(auth_middleware);

auto admin = api.group("/admin");   // inherits parent middleware
admin.use(admin_only_middleware);
admin.get("/stats", handler);       // → GET /api/v1/admin/stats
```

### Handler Parameters

All parameters are extracted from the request automatically based on their type.

| Type | Source |
|------|--------|
| `Request&` | Current request |
| `Response&` | Current response |
| `PathParam<T, "name">` | URL segment `:name` cast to T |
| `Query<T, "name">` | `?name=value` cast to T; absent → `T{}` |
| `Query<T, "name", "default">` | absent → converted from `"default"` |
| `Inject<T>` | Service from DI container; 500 if not registered |
| Any `SCHEMA` struct | Parsed from request body; 400/422 on failure |
| `Body<T>` | Same, with `operator bool` to check parse success |

Supported `T` for `PathParam` / `Query`: `int` `long` `float` `double` `bool` `std::string`.

### Schemas

```cpp
struct Product {
    int         id;
    std::string name;
    double      price;
    std::optional<std::string> description;  // absent or null → std::nullopt

    SCHEMA(Product, id, name, price, description)

    // Optional: business-rule validation.
    // Return one string per error; empty vector → 200, non-empty → 422.
    std::vector<std::string> validate() const {
        std::vector<std::string> errs;
        if (price <= 0)       errs.push_back("price: must be positive");
        if (name.empty())     errs.push_back("name: required");
        return errs;
    }
};

// Usage — no Body<> wrapper needed:
app.post("/products", [](Product p) -> nlohmann::json {
    return {{"id", p.id}, {"name", p.name}};
});

// Or use Body<T> when you need to distinguish parse failure from an empty result:
app.post("/products", [](Body<Product> body, Response& res) {
    if (!body) co_return;   // response already set to 400/422 by the framework
    return nlohmann::json{{"id", body->id}};
});
```

### Response

```cpp
res.status(201)
res.json({{"key", "value"}})
res.html("page.html")               // loads from templates dir
res.html("<h1>Hello</h1>")          // inline HTML
res.text("plain text")
res.send("raw body")
res.header("X-Custom", "value")
res.render("index.html", data)      // inja Jinja2 template
res.send_file("/abs/path/to/file")  // zero-copy sendfile(2) for non-TLS
```

Handlers can return a value instead of writing to `res` — auto-serialized to JSON:

```cpp
app.get("/a", [](Response& res) { res.json({{"x",1}}); });
app.get("/b", []() { return nlohmann::json{{"x",1}}; });
app.get("/c", []() -> Task<nlohmann::json> { co_return {{"x",1}}; });
app.get("/d", []() -> Product { return {1,"widget",9.99}; });  // SCHEMA struct
```

### Async & Cancellation

```cpp
// sleep() schedules via the thread-local event loop — no req.loop needed.
app.get("/delayed", []() -> Task<nlohmann::json> {
    co_await sleep(500);
    co_return nlohmann::json{{"done", true}};
});

// Check cancellation in long-running handlers so they exit when the client disconnects.
app.get("/poll", [](Request& req) -> Task<nlohmann::json> {
    for (int i = 0; i < 30; ++i) {
        co_await sleep(1000);
        if (req.is_cancelled()) co_return {};
        // ... poll a queue, check a DB, etc.
    }
    co_return nlohmann::json{{"cycles", 30}};
});
```

`sleep()` also wakes early when the connection is cancelled, so coroutines don't linger.

### Middleware

```cpp
// Custom middleware
app.use([](Request& req, Response& res, auto next) -> Task<void> {
    // Before handler
    co_await next();
    // After handler (headers not sent yet — safe to add them here)
    res.header("X-Request-ID", "...");
});

// Built-ins
app.use(osodio::logger());
app.use(osodio::logger(std::cerr));   // custom stream

app.use(osodio::compress());
app.use(osodio::compress({.min_size = 512, .level = 9, .brotli_quality = 5}));

app.use(osodio::cors());              // allow *
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

app.use(osodio::rate_limit({.requests = 60, .window_seconds = 60}));
app.use(osodio::rate_limit({
    .requests = 1000,
    // Key by API key header, falling back to IP
    .key_fn   = [](const Request& r) {
        return r.header("x-api-key").value_or(r.remote_ip);
    },
}));
```

### JWT

```cpp
#include <osodio/jwt.hpp>

// Sign
auto token = osodio::jwt::sign({
    {"sub",  "user-123"},
    {"role", "admin"},
    {"exp",  osodio::jwt::expires_in(3600)},  // seconds from now
}, "my-secret");

// Verify — throws osodio::JwtError on any failure
auto claims = osodio::jwt::verify(token, "my-secret");
std::string sub = claims.value("sub", "");

// HS256 middleware
app.use(osodio::jwt_auth("my-secret"));

// RS256 middleware (PEM public key)
app.use(osodio::jwt_auth_rsa(public_key_pem));

// Skip auth on specific routes
app.use(osodio::jwt_auth("secret", {
    .skip = [](const Request& req) {
        return req.path == "/auth/login" || req.path == "/health";
    },
}));

// Claims available in every handler after jwt_auth()
app.get("/me", [](Request& req) -> nlohmann::json {
    return {{"sub",  req.jwt_claims.value("sub",  "")},
            {"role", req.jwt_claims.value("role", "")}};
});
```

### Dependency Injection

```cpp
// Register a singleton — same shared_ptr for every request.
app.provide(std::make_shared<Database>(conn_str));

// Register a transient — new instance per Inject<T> resolution.
app.provide<Logger>([] { return std::make_shared<Logger>(); });

// Use in any handler:
app.get("/users", [](Inject<Database> db, Inject<Logger> log) -> Task<nlohmann::json> {
    log->info("listing users");
    auto rows = co_await db->query("SELECT id, name FROM users");
    co_return rows;
});
```

### Error Handling

```cpp
// Throw from any handler — caught by the framework, serialized to JSON.
throw osodio::not_found("user not found");        // 404
throw osodio::bad_request("invalid email");       // 400
throw osodio::unauthorized("login required");     // 401
throw osodio::forbidden("admin only");            // 403
throw osodio::conflict("already exists");         // 409
throw osodio::unprocessable("invalid data");      // 422
throw osodio::too_many_requests("slow down");     // 429
throw osodio::internal_error("db error");         // 500

// Per-code handlers
app.on_error(404, [](int, Request& req, Response& res) {
    res.json({{"error","Not Found"},{"path",req.path}});
});

// Catch-all for any 4xx/5xx
app.on_error([](int code, Request&, Response& res) {
    res.json({{"error","Something went wrong"},{"code",code}});
});
```

### Static Files & SPA

```cpp
app.serve_static("/static", "./public");          // prefix → directory
app.serve_static("/", "./dist", true);            // SPA: unknown paths → index.html

// ETag + Cache-Control set automatically.
// Hashed filenames (e.g. main.a1b2c3.js) → immutable for 1 year.
// All other files                          → max-age=3600, must-revalidate.
// Unchanged files                          → 304 Not Modified.
// Non-TLS: sendfile(2) — zero-copy kernel transfer.
```

### Server-Sent Events

```cpp
app.get("/events", [](Request& req, Response& res) -> Task<void> {
    auto sse = osodio::make_sse(res, req);   // writes headers immediately

    int seq = 0;
    while (sse.is_open()) {
        sse.send(std::to_string(seq++));                      // data: N\n\n
        sse.send_event("tick", "payload", "evt-" + std::to_string(seq)); // named + id
        sse.ping();                                           // keepalive comment
        co_await osodio::sleep(1000);
    }
});
```

- `is_open()` → false when the client disconnects (via `CancellationToken`)
- `sleep()` wakes early on disconnect so the loop exits cleanly
- The browser uses `Last-Event-ID` to resume from the right event after reconnect

### Multipart / File Uploads

```cpp
app.post("/upload", [](Request& req, Response& res) -> Task<void> {
    auto parts = osodio::parse_multipart(req);
    if (!parts) { res.status(400).json({{"error","not multipart"}}); co_return; }

    for (auto& p : *parts) {
        if (!p.filename.empty()) {
            // p.name, p.filename, p.content_type, p.body (raw bytes as std::string)
            std::ofstream f("uploads/" + p.filename, std::ios::binary);
            f.write(p.body.data(), static_cast<std::streamsize>(p.body.size()));
        } else {
            // Text field: p.name, p.body
        }
    }
    res.json({{"uploaded", static_cast<int>(parts->size())}});
});
```

### WebSockets

```cpp
app.ws("/chat", [](WSConnection ws) -> Task<void> {
    while (ws.is_open()) {
        auto msg = co_await ws.recv();   // suspends; resumes on message or disconnect
        if (!msg || msg->is_close()) break;

        if (msg->is_text())   ws.send("echo: " + msg->data);
        if (msg->is_binary()) ws.send_binary(msg->data.data(), msg->data.size());
    }
    // ws.close() called automatically when WSConnection is destroyed
});
```

### HTTPS + HTTP/2

```cpp
// Install: sudo apt install libssl-dev libnghttp2-dev
// Compile: cmake -DOSODIO_TLS=ON -DOSODIO_HTTP2=ON ...

app.tls("cert.pem", "key.pem");   // must be called before run()
app.run(443);
```

HTTP/2 is negotiated via ALPN during the TLS handshake. The same handler code
works for both HTTP/1.1 and HTTP/2 — including SSE and WebSockets.

---

## Building

Requires **CMake 3.20+**, **C++20** (GCC 11+ or Clang 13+), **Linux** (epoll), **zlib** (system).

```bash
# Minimal (HTTP only)
cmake -S . -B build
cmake --build build -j$(nproc)

# With HTTPS, HTTP/2, and Brotli
sudo apt install libssl-dev libnghttp2-dev libbrotli-dev
cmake -S . -B build -DOSODIO_TLS=ON -DOSODIO_HTTP2=ON -DOSODIO_BROTLI=ON
cmake --build build -j$(nproc)
```

All other dependencies are vendored in `third_party/` — no network access during cmake:

```
third_party/
  nlohmann/json.hpp     nlohmann/json v3.11.3
  simdjson.h / .cpp     simdjson v3.10.0  (amalgamated)
  inja.hpp              inja v3.4.0       (single-include)
  llhttp/               llhttp v9.2.1     (1 header + 3 .c files)
```

---

## Status

| Feature | |
|---------|--|
| Radix tree router (`:param`, `{param}`, `*`) | ✅ |
| Route groups with per-group middleware | ✅ |
| Handler dependency injection (all parameter types) | ✅ |
| `SCHEMA` — body auto-extract, no `Body<>` wrapper needed | ✅ |
| `std::optional<T>` fields — absent or null → `std::nullopt` | ✅ |
| `validate()` method — business-rule errors → 422 automatically | ✅ |
| `PathParam<T, "name">` | ✅ |
| `Query<T, "name", "default">` with default values | ✅ |
| `Inject<T>` — singleton + transient DI | ✅ |
| `Body<T>` — explicit body wrapper with `operator bool` | ✅ |
| Typed HTTP errors (`not_found()`, `bad_request()`, …) | ✅ |
| C++20 `Task<T>` coroutines | ✅ |
| `co_await sleep(ms)` — thread-local, no `req.loop` needed | ✅ |
| `req.is_cancelled()` — exits early on client disconnect | ✅ |
| epoll event loop, non-blocking I/O, EPOLLOUT backpressure | ✅ |
| HTTP/1.1 keep-alive, pipelining-safe parser | ✅ |
| Header timeout 5 s (Slowloris), re-armed on keep-alive | ✅ |
| Handler + write timeout 30 s | ✅ |
| Connection limit (`app.max_connections`) | ✅ |
| `compress()` — gzip + Brotli, negotiated via Accept-Encoding | ✅ |
| `cors()` — full preflight, allow-list, credentials | ✅ |
| `logger()` — method, path, status, duration | ✅ |
| `helmet()` — CSP, HSTS, X-Frame-Options, X-Content-Type-Options | ✅ |
| `rate_limit()` — fixed-window per IP or custom key | ✅ |
| `jwt_auth()` / `jwt::sign` / `jwt::verify` — HS256 + RS256 | ✅ |
| Static files — MIME, ETag, 304, sendfile(2), SPA fallback | ✅ |
| SSE — `make_sse()`, named events, ping, auto-disconnect | ✅ |
| WebSockets — RFC 6455, binary/text, ping/pong, fragmentation | ✅ |
| Multipart/form-data — `parse_multipart()`, file + text fields | ✅ |
| HTML templates via inja (Jinja2-compatible) | ✅ |
| OpenAPI 3.0 + Swagger UI at `/docs` | ✅ |
| `enable_health()` + `enable_metrics()` (Prometheus) | ✅ |
| Global + per-code error handlers | ✅ |
| Multi-thread — one epoll loop per core, SO_REUSEPORT | ✅ |
| Graceful shutdown — SIGTERM drains, second signal forces exit | ✅ |
| HTTPS / TLS via OpenSSL | ✅ |
| HTTP/2 via nghttp2 with ALPN | ✅ |
| Brotli compression | ✅ |
| Vendored deps — no cmake network access | ✅ |
