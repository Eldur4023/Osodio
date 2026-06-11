# Osodio

C++20 async HTTP framework. Born from frustration with Node.js and the wall that Flask/FastAPI hit at scale.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

struct CreatePost {
    std::string title;
    std::string content;
    std::optional<std::string> tags;
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

    api.post("/posts", [](CreatePost post, Request& req) -> nlohmann::json {
        std::string author = req.jwt_claims.value("sub", "");
        return {{"id", 1}, {"title", post.title}, {"author", author}};
    });

    api.get("/posts/:id", [](PathParam<int,"id"> id) -> Task<nlohmann::json> {
        co_await sleep(0);
        co_return nlohmann::json{{"id", (int)id}, {"title", "Hello world"}};
    });

    app.run(8080);
}
```

---

## Why does this exist?

### JavaScript is a bad language and its ecosystem is insufferable

Before getting to Node.js specifically: JavaScript is a genuinely bad language. Not "imperfect" or "quirky" — bad. The type coercion rules are a horror show (`[] + {} === "[object Object]"`, `{} + [] === 0`, `0.1 + 0.2 !== 0.3`). `this` changes meaning depending on how a function is called, not where it's defined. There are two different null values (`null` and `undefined`) with subtly different behaviors across every API. `var` has function scope instead of block scope, a mistake so bad they had to add `let` and `const` to paper over it. `==` has a 30-entry comparison table of implicit coercions; the only correct advice is to never use it.

The ecosystem built around this has metastasized into something genuinely painful to work in. A fresh `create-react-app` pulls 1500+ packages and 300MB of `node_modules` for a Hello World. Projects routinely depend on packages with a single exported function (left-pad, is-odd, is-even, is-number — these are real packages with millions of weekly downloads). The supply chain attack surface is enormous: any of those 1500 packages can run arbitrary code on install via `postinstall` scripts. NPM's security record speaks for itself.

TypeScript patches the type system but adds a compilation step, a separate config file (`tsconfig.json`) that interacts badly with bundler configs (`webpack.config.js`, `vite.config.ts`, `babel.config.json`, `.babelrc`), and a toolchain that can spend more time type-checking than your program spends running. The types themselves are structural and unsound — you can write TypeScript that passes `tsc` and still crashes at runtime because `any` propagates silently and `JSON.parse` returns `any`.

Prettier, ESLint, Husky, lint-staged, commitlint — hours lost configuring tools that exist purely because the language doesn't enforce anything on its own.

### The Node.js performance problem

Node.js is single-threaded. One CPU core. That's not an architectural choice you can tune away — it's fundamental to how V8 and libuv work. The event loop processes one callback at a time, and any CPU-bound work (JSON serialization at scale, image processing, crypto, request validation) blocks every other request behind it.

The workarounds are well-known and all painful: `cluster` forks N processes and now you need IPC for shared state; `worker_threads` gets you parallelism but with a deliberately restricted API that can't touch the DOM or most native modules; `pm2` is just `cluster` with a process manager on top.

Then there's V8 GC. At high request rates the garbage collector pauses become visible in latency percentiles — not every request, but the p99 and p999 are ugly. You can tune heap sizes and generation ratios but you're fighting a runtime that wasn't designed for this workload. And TypeScript adds a compile step that turns every hot reload into a multi-second wait in large codebases.

Osodio runs one native OS thread per CPU core with no GIL, no GC, no IPC for shared state, and no compile step between your code change and a running binary.

### The Python problem

Flask is synchronous. Every request occupies a thread for its entire lifetime. With uvicorn + FastAPI you get async I/O, which helps when handlers are I/O-bound, but the Python GIL still means only one thread executes Python bytecode at any given moment. `multiprocessing` gets you real parallelism but at the cost of forking the entire interpreter — memory doubles, shared state becomes a coordination problem, and startup time grows.

The numbers matter here. A vanilla FastAPI endpoint doing nothing but JSON serialization tops out around 30–50k req/s on an 8-core machine. Not because the networking is slow — uvicorn and httptools are fast — but because Python's object model, attribute lookup, and GC have a fixed cost per request that compounds. Add a database call, some Pydantic validation, and real business logic and that number drops further.

Osodio handles the same workload with no interpreter overhead: request parsing is llhttp (the same parser Node uses), JSON is nlohmann or simdjson, and the entire hot path from accept() to response write is native code with C++20 coroutines that compile to state machines with zero runtime cost.

### Design choice: Flask/FastAPI syntax, C++ speed

The goal was never to reinvent the wheel of API design. FastAPI got the developer experience right: declare what your handler needs as function parameters, let the framework figure out where to get it. No `request.json()`, no manual header parsing, no boilerplate — just a function that says what it wants and receives it.

That's the syntax we wanted. The problem is that Python can't deliver the performance.

C++ is not Python. The syntax limitations are real and there's no point pretending otherwise — no decorators, no runtime introspection of parameter names, no duck typing, no optional typing that the framework reads at runtime. But C++ templates and parameter packs get surprisingly close, and that's what Osodio is: an honest attempt to bring FastAPI's ergonomics into a language that can actually handle hundreds of thousands of requests per second without forking twenty processes.

The comparison is direct:

```python
# FastAPI
@app.get("/articles/{id}")
async def get_article(
    id: int,
    db: Database = Depends(get_db),
    page: int = Query(default=1),
):
    ...
```

```cpp
// Osodio
app.get("/articles/:id", [](
    PathParam<int, "id"> id,
    Inject<Database>     db,
    Query<int, "page", "1"> page
) -> Task<nlohmann::json> {
    co_return co_await db->get(id);
});
```

```python
# FastAPI
class CreatePost(BaseModel):
    title: str
    content: str
    tags: Optional[list[str]] = None

    @validator("title")
    def title_length(cls, v):
        if len(v) < 3:
            raise ValueError("min 3 characters")
        return v

@app.post("/posts")
async def create_post(post: CreatePost, db: Database = Depends(get_db)):
    ...
```

```cpp
// Osodio
struct CreatePost {
    std::string title;
    std::string content;
    std::optional<std::vector<std::string>> tags;
    SCHEMA(CreatePost, title, content, tags)

    std::vector<std::string> validate() const {
        if (title.size() < 3) return {"title: min 3 characters"};
        return {};
    }
};

app.post("/posts", [](CreatePost post, Inject<Database> db) -> Task<nlohmann::json> {
    co_return co_await db->insert(post);
});
```

The `SCHEMA` macro is the main compromise — and a temporary one. FastAPI reads Pydantic field names via Python's type system at runtime; C++ erases names at compile time, so they have to be declared explicitly for now. Everything else maps almost one-to-one: `Depends()` → `Inject<T>`, `Query(default=...)` → `Query<T, "name", "default">`, `Optional[T]` → `std::optional<T>`, `async def` + `await` → coroutine + `co_await`. The 422 validation response, automatic JSON body parsing, and path parameter coercion all work the same way.

C++26 static reflection (P2996) changes this completely. With `std::meta::members_of` the framework can enumerate struct fields and their names at compile time — no macro, no boilerplate, zero runtime cost. The same struct Pydantic inspects dynamically, C++26 will inspect statically. `SCHEMA` will disappear entirely: declare the struct, use it as a handler parameter, done. Osodio is being built with that future in mind as compiler support rolls out.

It's not identical yet. But it's close enough that someone who knows FastAPI can read Osodio handlers without learning a new mental model, and it's getting closer with every C++ standard.

### What Osodio actually does differently

**True multi-core without forking.** One epoll event loop per hardware thread, all bound to the same port via `SO_REUSEPORT`. The kernel distributes connections across cores. Shared state (DI services, counters) is managed through `std::shared_ptr` and `std::atomic` — no IPC, no serialization, no process boundary.

**Async with no runtime.** C++20 coroutines are a language feature, not a library. `co_await sleep(500)` compiles to a state machine that suspends the current frame and returns control to the event loop — no thread switch, no allocation beyond the initial frame, no scheduler that can be starved. The same coroutine mechanism drives route handlers, middleware chains, WebSocket loops, and SSE streams.

**Zero-copy where it matters.** Static file serving uses `sendfile(2)` — the kernel reads the file descriptor and writes to the socket without the data ever entering userspace. HTTP/2 uses nghttp2's `NGHTTP2_DATA_FLAG_NO_COPY` data provider for the same effect.

**No hidden costs.** No reflection framework scanning classes at startup. No runtime type erasure for handler dispatch. No dynamic dispatch in the middleware chain — it unrolls at compile time through template instantiation.

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
| **Middleware** | `logger()`, `cors()`, `compress()`, `helmet()`, `rate_limit()`, `jwt_auth()`, `csrf()` built-in |
| **JWT** | `jwt::sign` / `jwt::verify` / `jwt_auth()` — HS256 and RS256, claim validation |
| **Cookies** | `res.cookie()`, `req.cookie()`, `SameSite`, `Secure`, `HttpOnly`, `Max-Age` |
| **CSRF** | Double-submit cookie pattern, stateless, `RAND_bytes` token, `CRYPTO_memcmp` validation |
| **Dependency injection** | `app.provide<T>(...)` / `Inject<T>` in any handler — singleton and transient |
| **Static files** | `serve_static(prefix, dir, spa)` — MIME, ETag, Cache-Control, 304, `sendfile(2)`, SPA fallback |
| **SSE** | `make_sse(res, req)` — `text/event-stream`, named events, keepalive pings, auto-disconnect |
| **WebSockets** | `app.ws(path, handler)` — RFC 6455, binary/text frames, ping/pong, fragmentation, origin check |
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

    app.post("/auth/login", [](LoginRequest req) -> nlohmann::json {
        if (req.username != "alice" || req.password != "hunter2")
            throw osodio::unauthorized("invalid credentials");

        auto token = jwt::sign({
            {"sub",  req.username},
            {"role", "editor"},
            {"exp",  jwt::expires_in(86400)},
        }, JWT_SECRET);

        return {{"token", token}, {"expires_in", 86400}};
    });

    auto api = app.group("/api/v1");
    api.use(osodio::jwt_auth(JWT_SECRET, {
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

    app.on_error(404, [](int, Request& req, Response& res) {
        res.json({{"error", "Not Found"}, {"path", req.path}});
    });
    app.on_error([](int code, Request&, Response& res) {
        res.json({{"error", "Something went wrong"}, {"code", code}});
    });

    app.enable_docs();
    app.run(8080);
}
```

---

### Session-based auth with CSRF protection

Cookie sessions + CSRF protection for browser-facing apps. The double-submit pattern is stateless — no session store needed. Requires `cmake -DOSODIO_TLS=ON`.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

int main() {
    App app;

    app.use(osodio::logger());
    app.use(osodio::helmet());

    // csrf() must come before route handlers.
    // Automatically skips Bearer-token requests and safe methods (GET/HEAD/OPTIONS).
    // Browser JS reads the csrf_token cookie and echoes it in X-CSRF-Token.
    app.use(osodio::csrf());

    app.post("/login", [](Request& req, Response& res) {
        auto f = req.form();
        if (f["username"] != "alice" || f["password"] != "hunter2") {
            res.status(401).json({{"error", "invalid credentials"}});
            return;
        }

        // Set a session cookie — HttpOnly so JS can't read it, Secure in prod.
        res.cookie("session", "opaque-session-token", {
            .secure    = true,
            .http_only = true,
            .same_site = SameSite::Lax,
        });
        res.json({{"ok", true}});
    });

    app.post("/logout", [](Request& req, Response& res) {
        // Overwrite the cookie with Max-Age=0 to delete it.
        res.clear_cookie("session", {.secure = true});
        res.json({{"ok", true}});
    });

    app.get("/profile", [](Request& req, Response& res) {
        auto session = req.cookie("session");
        if (!session) {
            res.status(401).json({{"error", "not logged in"}});
            return;
        }
        res.json({{"user", "alice"}});
    });

    app.tls("cert.pem", "key.pem");
    app.run(443);
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

            // Never trust user-supplied paths — take only the filename component.
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

    app.serve_static("/static/uploads", "./uploads");
    app.run(8080);
}
```

---

### Live updates with Server-Sent Events

A dashboard that receives real-time metrics from the server. The browser reconnects automatically using `Last-Event-ID` if the connection drops.

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
    app.use(osodio::cors());

    app.get("/metrics/live", [](Request& req, Response& res,
                                Inject<Metrics> m) -> Task<void> {
        auto sse = osodio::make_sse(res, req);

        sse.send_event("snapshot", nlohmann::json{
            {"requests", m->requests.load()},
            {"errors",   m->errors.load()},
            {"ts",       std::time(nullptr)},
        }.dump(), std::to_string(std::time(nullptr)));

        int tick = 0;
        while (sse.is_open()) {
            co_await osodio::sleep(2000);
            if (req.is_cancelled()) break;

            sse.send_event("delta", nlohmann::json{
                {"requests", m->requests.load()},
                {"errors",   m->errors.load()},
                {"ts",       std::time(nullptr)},
            }.dump(), std::to_string(++tick));

            if (tick % 10 == 0) sse.ping("keepalive");
        }
    });

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

Multiple browser tabs share a counter. Any tab can increment or reset; all connected clients see the change immediately.

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

std::atomic<int> g_counter{0};

int main() {
    App app;
    app.use(osodio::cors());

    app.ws("/counter", [](WSConnection ws) -> Task<void> {
        ws.send(nlohmann::json{{"count", g_counter.load()}}.dump());

        while (ws.is_open()) {
            auto msg = co_await ws.recv();
            if (!msg || msg->is_close()) break;
            if (!msg->is_text()) continue;

            auto data = nlohmann::json::parse(msg->data, nullptr, false);
            if (data.is_discarded()) continue;

            std::string action = data.value("action", "");
            if (action == "increment")
                ws.send(nlohmann::json{{"count", ++g_counter}}.dump());
            else if (action == "decrement")
                ws.send(nlohmann::json{{"count", --g_counter}}.dump());
            else if (action == "reset") {
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

```cpp
#include <osodio/osodio.hpp>
using namespace osodio;

int main() {
    App app;

    app.use(osodio::logger());
    app.use(osodio::compress());
    app.use(osodio::helmet());
    app.use(osodio::cors({.origins = {"https://myapp.com"}}));

    auto api = app.group("/api/v1");
    api.use(osodio::jwt_auth("secret"));
    api.get("/me", [](Request& req) -> nlohmann::json {
        return {{"sub", req.jwt_claims.value("sub", "")},
                {"role", req.jwt_claims.value("role", "")}};
    });

    // SPA fallback: unmatched paths → ./dist/index.html.
    // Hashed assets (app.abc123.js) → Cache-Control: immutable for 1 year.
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
    std::optional<std::string> description;

    SCHEMA(Product, id, name, price, description)

    std::vector<std::string> validate() const {
        std::vector<std::string> errs;
        if (price <= 0)    errs.push_back("price: must be positive");
        if (name.empty())  errs.push_back("name: required");
        return errs;
    }
};

app.post("/products", [](Product p) -> nlohmann::json {
    return {{"id", p.id}, {"name", p.name}};
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

// Cookies
res.cookie("name", "value");
res.cookie("session", token, {
    .path      = "/",
    .secure    = true,
    .http_only = true,
    .same_site = SameSite::Strict,
    .max_age   = 86400,
});
res.clear_cookie("name");           // Set-Cookie with Max-Age=0
```

Handlers can return a value instead of writing to `res` — auto-serialized to JSON:

```cpp
app.get("/a", [](Response& res) { res.json({{"x",1}}); });
app.get("/b", []() { return nlohmann::json{{"x",1}}; });
app.get("/c", []() -> Task<nlohmann::json> { co_return {{"x",1}}; });
app.get("/d", []() -> Product { return {1,"widget",9.99}; });
```

### Cookies

```cpp
// Read a cookie from the request (lazy-parsed, O(1) after first call)
auto session = req.cookie("session");
if (!session) { /* not set */ }

// SameSite enum: SameSite::Strict | SameSite::Lax | SameSite::None
// SameSite::None forces Secure=true automatically.

// Write a cookie to the response (multiple cookies, each a Set-Cookie header)
res.cookie("pref", "dark", {
    .path      = "/",
    .domain    = "example.com",
    .max_age   = 365 * 86400,
    .secure    = true,
    .http_only = false,         // JS-readable preference cookie
    .same_site = SameSite::Lax,
});
```

### Async & Cancellation

```cpp
app.get("/delayed", []() -> Task<nlohmann::json> {
    co_await sleep(500);
    co_return nlohmann::json{{"done", true}};
});

app.get("/poll", [](Request& req) -> Task<nlohmann::json> {
    for (int i = 0; i < 30; ++i) {
        co_await sleep(1000);
        if (req.is_cancelled()) co_return {};
    }
    co_return nlohmann::json{{"cycles", 30}};
});
```

`sleep()` also wakes early when the connection is cancelled, so coroutines don't linger.

### Middleware

```cpp
// Custom middleware
app.use([](Request& req, Response& res, auto next) -> Task<void> {
    co_await next();
    res.header("X-Request-ID", "...");
});

// Built-ins
app.use(osodio::logger());

// The logger() middleware writes through the global logger — configure file
// output, rotation and the per-minute performance report once at startup:
osodio::log().configure({
    .dir = "./logs", .max_file_size = 10 * 1024 * 1024, .performance = true,
});
osodio::log().info("general-purpose logging, not just HTTP");

app.use(osodio::compress());
app.use(osodio::compress({.min_size = 512, .level = 9, .brotli_quality = 5}));

app.use(osodio::cors());
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
    .key_fn   = [](const Request& r) {
        return r.header("x-api-key").value_or(r.remote_ip);
    },
}));

// CSRF — double-submit cookie (requires OSODIO_TLS=ON)
// No-op for Bearer-token clients (Authorization: header bypasses it).
// No-op for safe methods (GET, HEAD, OPTIONS, TRACE).
app.use(osodio::csrf());
app.use(osodio::csrf({
    .cookie_name = "csrf_token",
    .header_name = "x-csrf-token",
    .cookie_opts = {.secure = true, .same_site = SameSite::Strict},
    // Skip webhook endpoints that use HMAC signatures instead
    .skip = [](const Request& r) {
        return r.path.starts_with("/webhooks/");
    },
}));
```

### JWT

```cpp
#include <osodio/jwt.hpp>

auto token = osodio::jwt::sign({
    {"sub",  "user-123"},
    {"role", "admin"},
    {"exp",  osodio::jwt::expires_in(3600)},
}, "my-secret");

auto claims = osodio::jwt::verify(token, "my-secret");
std::string sub = claims.value("sub", "");

app.use(osodio::jwt_auth("my-secret"));
app.use(osodio::jwt_auth_rsa(public_key_pem));
app.use(osodio::jwt_auth("secret", {
    .skip = [](const Request& req) {
        return req.path == "/auth/login" || req.path == "/health";
    },
}));

app.get("/me", [](Request& req) -> nlohmann::json {
    return {{"sub",  req.jwt_claims.value("sub",  "")},
            {"role", req.jwt_claims.value("role", "")}};
});
```

### Dependency Injection

```cpp
app.provide(std::make_shared<Database>(conn_str));
app.provide<Logger>([] { return std::make_shared<Logger>(); });

app.get("/users", [](Inject<Database> db, Inject<Logger> log) -> Task<nlohmann::json> {
    log->info("listing users");
    auto rows = co_await db->query("SELECT id, name FROM users");
    co_return rows;
});
```

### Error Handling

```cpp
throw osodio::not_found("user not found");        // 404
throw osodio::bad_request("invalid email");       // 400
throw osodio::unauthorized("login required");     // 401
throw osodio::forbidden("admin only");            // 403
throw osodio::conflict("already exists");         // 409
throw osodio::unprocessable("invalid data");      // 422
throw osodio::too_many_requests("slow down");     // 429
throw osodio::internal_error("db error");         // 500

app.on_error(404, [](int, Request& req, Response& res) {
    res.json({{"error","Not Found"},{"path",req.path}});
});
app.on_error([](int code, Request&, Response& res) {
    res.json({{"error","Something went wrong"},{"code",code}});
});
```

### Static Files & SPA

```cpp
app.serve_static("/static", "./public");
app.serve_static("/", "./dist", true);   // SPA: unknown paths → index.html

// ETag + Cache-Control set automatically.
// Hashed filenames (e.g. main.a1b2c3.js) → immutable for 1 year.
// All other files                          → max-age=3600, must-revalidate.
// Unchanged files                          → 304 Not Modified.
// Non-TLS: sendfile(2) — zero-copy kernel transfer.
// Dotfiles (.env, .git, .htaccess) → 404, never served.
```

### Server-Sent Events

```cpp
app.get("/events", [](Request& req, Response& res) -> Task<void> {
    auto sse = osodio::make_sse(res, req);

    int seq = 0;
    while (sse.is_open()) {
        sse.send(std::to_string(seq++));
        sse.send_event("tick", "payload", "evt-" + std::to_string(seq));
        sse.ping();
        co_await osodio::sleep(1000);
    }
});
```

### WebSockets

```cpp
// Basic echo
app.ws("/chat", [](WSConnection ws) -> Task<void> {
    while (ws.is_open()) {
        auto msg = co_await ws.recv();
        if (!msg || msg->is_close()) break;
        if (msg->is_text())   ws.send("echo: " + msg->data);
        if (msg->is_binary()) ws.send_binary(msg->data.data(), msg->data.size());
    }
});

// Restrict to known origins (Cross-Site WebSocket Hijacking protection)
app.ws("/secure-chat", handler, WSOptions{
    .allowed_origins = {"https://myapp.com", "https://staging.myapp.com"},
});
```

### HTTPS + HTTP/2

```cpp
// Install: sudo apt install libssl-dev libnghttp2-dev
// Compile: cmake -DOSODIO_TLS=ON -DOSODIO_HTTP2=ON ...

app.tls("cert.pem", "key.pem");
app.run(443);
```

HTTP/2 is negotiated via ALPN during the TLS handshake. The same handler code works for both HTTP/1.1 and HTTP/2 — including SSE and WebSockets (RFC 8441).

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
| `logger()` — method, path, status, duration; rotating file output + performance report via `log().configure()` | ✅ |
| `helmet()` — CSP, HSTS, X-Frame-Options, X-Content-Type-Options | ✅ |
| `rate_limit()` — fixed-window per IP or custom key | ✅ |
| `jwt_auth()` / `jwt::sign` / `jwt::verify` — HS256 + RS256 | ✅ |
| Cookies — `res.cookie()`, `req.cookie()`, `SameSite`, `Secure`, `HttpOnly` | ✅ |
| CSRF — double-submit cookie, stateless, constant-time comparison | ✅ |
| Static files — MIME, ETag, 304, sendfile(2), SPA fallback, dotfile blocking | ✅ |
| SSE — `make_sse()`, named events, ping, auto-disconnect | ✅ |
| WebSockets — RFC 6455, binary/text, ping/pong, fragmentation, origin check | ✅ |
| Multipart/form-data — `parse_multipart()`, file + text fields | ✅ |
| HTML templates via inja (Jinja2-compatible) | ✅ |
| OpenAPI 3.0 + Swagger UI at `/docs` | ✅ |
| `enable_health()` + `enable_metrics()` (Prometheus) | ✅ |
| Global + per-code error handlers | ✅ |
| Multi-thread — one epoll loop per core, SO_REUSEPORT | ✅ |
| Graceful shutdown — SIGTERM drains, second signal forces exit | ✅ |
| HTTPS / TLS via OpenSSL | ✅ |
| HTTP/2 via nghttp2 with ALPN | ✅ |
| HTTP/2 WebSockets (RFC 8441) | ✅ |
| HTTP/2 Rapid Reset mitigation (CVE-2023-44487) | ✅ |
| Brotli compression | ✅ |
| Vendored deps — no cmake network access | ✅ |
