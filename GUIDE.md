# Osodio — Developer Guide

This document covers the internal architecture and every public API in detail.  
It is intended for contributors to the library and as a reference for LLMs generating code with Osodio.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Request Lifecycle](#2-request-lifecycle)
3. [File Structure](#3-file-structure)
4. [App](#4-app)
5. [Routing](#5-routing)
6. [Handler Injection System](#6-handler-injection-system)
7. [Request](#7-request)
8. [Response](#8-response)
9. [JSON Serialization — OSODIO_SCHEMA](#9-json-serialization--osodio_schema)
10. [Validation — OSODIO_VALIDATE](#10-validation--osodio_validate)
11. [Async — Task\<T\> and Coroutines](#11-async--taskt-and-coroutines)
12. [Middleware](#12-middleware)
13. [Static Files](#13-static-files)
14. [Error Handlers](#14-error-handlers)
15. [HTML Templates](#15-html-templates)
16. [Event Loop](#16-event-loop)
17. [HTTP Parser](#17-http-parser)
18. [TCP Server](#18-tcp-server)
19. [HTTP Connection](#19-http-connection)
20. [How to Extend Osodio](#20-how-to-extend-osodio)

---

## 1. Architecture Overview

Osodio is a single-threaded, epoll-based HTTP/1.1 server. It runs on Linux.

```
┌─────────────────────────────────────────────────────────────┐
│  App (user-facing API)                                       │
│   - registers routes, middleware, error handlers             │
│   - builds a DispatchFn closure                              │
│   - starts the event loop                                    │
└──────────────────────────────┬──────────────────────────────┘
                               │ DispatchFn
┌──────────────────────────────▼──────────────────────────────┐
│  core::TcpServer                                             │
│   - creates a listening socket (SO_REUSEADDR, TCP_NODELAY)   │
│   - registers accept callback with the EventLoop             │
└──────────────────────────────┬──────────────────────────────┘
                               │ accept(2) → new fd
┌──────────────────────────────▼──────────────────────────────┐
│  http::HttpConnection  (one per TCP connection)              │
│   - registered in epoll for EPOLLIN                          │
│   - feeds bytes into HttpParser                              │
│   - calls DispatchFn when a full request is parsed           │
│   - writes the response back to the socket                   │
└──────────────────────────────┬──────────────────────────────┘
                               │ ParsedRequest
┌──────────────────────────────▼──────────────────────────────┐
│  DispatchFn  (built by App::run)                             │
│   1. Set req.loop and res.templates_dir                      │
│   2. Check static file mounts (bypass middlewares)           │
│   3. Run middleware chain                                     │
│   4. Match route in Router → call Handler                    │
│   5. Run error handlers if status >= 400                     │
└──────────────────────────────┬──────────────────────────────┘
                               │ Request& + Response&
┌──────────────────────────────▼──────────────────────────────┐
│  Router → HandlerTraits<F>::call                             │
│   - extracts typed arguments from Request via extractors     │
│   - calls the user's lambda                                  │
│   - handles sync return, void return, or Task<T> (async)     │
└─────────────────────────────────────────────────────────────┘
```

**Key design decisions:**

- **Single-threaded**: The event loop runs on one thread. `SleepAwaitable` uses a detached thread internally to post a resume callback, but the handlers themselves always execute on the loop thread.
- **`shared_ptr<State>` in Response**: Async handlers capture `Response` by value. All copies share state so the completion callback can write the body from any capture.
- **`DispatchFn`**: A `std::function<void(Request&, Response&)>`. Decouples the connection layer from the application layer; the TCP/HTTP layer knows nothing about routing.
- **Middleware chain as `shared_ptr<std::function<void(size_t)>>`**: Allows the chain to be safely captured by value in the `next` closures without dangling references.

---

## 2. Request Lifecycle

```
1.  epoll_wait() wakes up → TcpServer::on_accept()
2.  accept(2) returns new fd
3.  HttpConnection created, registered in epoll (EPOLLIN | EPOLLET)
4.  epoll_wait() wakes up → HttpConnection::on_event(EPOLLIN)
5.  HttpConnection::do_read() → read(2) → HttpParser::feed()
6.  HttpParser accumulates bytes; when complete → calls on_complete callback
7.  HttpConnection::dispatch(ParsedRequest)
      - builds Request and Response objects on the stack
      - calls DispatchFn(req, res)
8.  DispatchFn:
      a. Sets req.loop, res.templates_dir
      b. Checks static mounts → if match, writes response, returns
      c. Builds middleware call_next chain
      d. call_next(0) → runs middleware[0] → ... → Router::match()
      e. Router returns handler + extracted params
      f. HandlerTraits::call() → extracts arguments → calls lambda
      g. After chain: if status >= 400, calls error handler
9.  If sync: HttpConnection::finish_dispatch()
      - determines keep-alive from HTTP version + Connection header
      - calls Response::build() → writes HTTP/1.1 response string
      - send_response() → write(2) in a loop
      - if not keep-alive: close the connection
10. If async (Task<T>):
      - Response::mark_async() called before h.resume()
      - Coroutine suspends at co_await
      - Event loop continues serving other connections
      - When coroutine resumes and co_returns:
          * on_complete callback fires: res.json(val), res.complete_async()
          * complete_async() calls on_complete_cb
          * http_connection posts finish_dispatch to the event loop
      - finish_dispatch() runs on the loop thread → same as sync path
```

---

## 3. File Structure

```
osodio/
├── include/osodio/
│   ├── osodio.hpp          Single public include
│   ├── app.hpp             App class (routing, middleware, run)
│   ├── request.hpp         Request struct
│   ├── response.hpp        Response class (builder API)
│   ├── router.hpp          Router class (radix tree, template add<F>)
│   ├── handler_traits.hpp  HandlerTraits, extractors, PathParam, Body, Query
│   ├── task.hpp            Task<T>, Task<void>, SleepAwaitable
│   ├── schema.hpp          OSODIO_SCHEMA, OSODIO_VALIDATE macros
│   ├── validation.hpp      ValidationError, validator builders
│   ├── types.hpp           Handler, Middleware, DispatchFn, ErrorHandler typedefs
│   └── core/
│       └── event_loop.hpp  EventLoop (epoll + task queue)
│
├── src/
│   ├── app.cpp             App::run, static file serving, dispatch closure
│   ├── router.cpp          Radix tree implementation
│   ├── core/
│   │   ├── event_loop.cpp  epoll_wait loop, eventfd wakeup
│   │   ├── tcp_server.hpp  TcpServer declaration
│   │   └── tcp_server.cpp  listen socket, on_accept
│   └── http/
│       ├── http_parser.hpp  HttpParser declaration
│       ├── http_parser.cpp  Line-based incremental HTTP/1.1 parser
│       ├── http_connection.hpp
│       └── http_connection.cpp  Per-connection read/write, dispatch bridge
│
├── third_party/
│   └── nlohmann/
│       └── json.hpp        nlohmann/json v3.11.3 (vendored)
│
├── templates/              Default template directory (shipped with example)
├── main.cpp                Example application
├── CMakeLists.txt
├── README.md
└── GUIDE.md                This file
```

---

## 4. App

**Header**: `include/osodio/app.hpp`  
**Implementation**: `src/app.cpp`

`App` is the user-facing entry point. It owns the router, middleware list, static mounts, and error handlers.

```cpp
class App {
public:
    // Route registration
    template<typename F> App& get   (std::string path, F&& h);
    template<typename F> App& post  (std::string path, F&& h);
    template<typename F> App& put   (std::string path, F&& h);
    template<typename F> App& patch (std::string path, F&& h);
    template<typename F> App& del   (std::string path, F&& h);
    template<typename F> App& any   (std::string path, F&& h);  // matches all methods

    // Middleware
    App& use(Middleware m);

    // Static file serving
    App& serve_static(std::string url_prefix, std::string fs_root);

    // Error handlers
    App& on_error(int code, ErrorHandler h);   // specific status code
    App& on_error(ErrorHandler h);             // catch-all for any 4xx/5xx

    // Templates base directory (default: "./templates")
    App& set_templates(std::string dir);

    // Start the server (blocks until SIGINT/SIGTERM)
    void run(uint16_t port = 5000);
    void run(const std::string& host, uint16_t port);
};
```

All `App&` methods return `*this` for chaining:

```cpp
app.use(logger)
   .serve_static("/static", "./public")
   .on_error(404, not_found_handler)
   .get("/ping", ping_handler)
   .run(8080);
```

`App::run` builds a `DispatchFn` closure that captures all registered state, then creates the `TcpServer` and starts `EventLoop::run()`. The process is blocked until a signal is received.

---

## 5. Routing

**Header**: `include/osodio/router.hpp`  
**Implementation**: `src/router.cpp`

The router uses a radix tree (trie) with three node types:

| Node type | Pattern | Example match |
|-----------|---------|---------------|
| `STATIC`   | Literal segment | `/users` matches `/users` |
| `PARAM`    | `:name` segment | `:id` matches `42`, captures `id=42` |
| `WILDCARD` | `*` segment | `*` matches everything after the prefix |

Route patterns are split on `/`. Each segment becomes a node. PARAM and WILDCARD nodes are stored as children of their parent and tried only when no STATIC child matches.

**Priority**: STATIC > PARAM > WILDCARD.

The `{id}` syntax is normalized to `:id` before insertion.

```cpp
router.add("GET", "/users/:id/posts/:pid", handler);
// match("/users/42/posts/7") → params = {{"id","42"}, {"pid","7"}}
```

`Router::add<F>` wraps `F` in a type-erased `Handler` using `HandlerTraits`:

```cpp
template<typename F>
void add(std::string method, std::string pattern, F&& handler) {
    auto wrapped = [h = std::forward<F>(handler)](Request& req, Response& res) mutable {
        HandlerTraits<F>::call(h, req, res);
    };
    add_internal(std::move(method), std::move(pattern), std::move(wrapped));
}
```

---

## 6. Handler Injection System

**Header**: `include/osodio/handler_traits.hpp`

This is the core of the ergonomic API. `HandlerTraits<F>` deduces the parameter types of a callable at compile time and calls the appropriate extractor for each.

### How it works

```cpp
template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...) const> {
    template<typename F>
    static void call(F&& f, const Request& req, Response& res) {
        auto args = std::tuple<Args...>{extractor<Args>::extract(req, res)...};
        if (res.status_code() >= 400) return;  // extractor set an error
        // dispatch based on return type R...
    }
};
```

The `Args...` pack is deduced from the lambda's `operator()`. Each `Args` is resolved by `extractor<T>::extract(req, res)`.

If any extractor sets a 4xx status (e.g., `Body<T>` with bad JSON), the handler is not called and the error response is sent as-is.

### Extractors

```cpp
// Default: handles Request&, Response&, or anything constructible from defaults
template<typename T>
struct extractor {
    static T extract(const Request& req, Response& res);
};

// PathParam<T, Name>: reads req.params[Name], converts to T
template<typename T, fixed_string Name>
struct extractor<PathParam<T, Name>>;

// Query<T, Name>: reads req.query[Name], converts to T
template<typename T, fixed_string Name>
struct extractor<Query<T, Name>>;

// Body<T>: parses req.body as JSON, optionally validates
template<typename T>
struct extractor<Body<T>>;
```

The `Body<T>` extractor:
1. Parses `req.body` with `nlohmann::json::parse`.
2. Calls `j.get<T>()` (requires `OSODIO_SCHEMA` or manual `from_json`).
3. If `has_validate<T>` is true (i.e., `OSODIO_VALIDATE` was used), calls `validate(val)`.
4. On `ValidationError`: sets `res.status(422)`, writes error JSON, returns default `Body<T>{}`.
5. On any other exception: sets `res.status(400)`.

### Return type dispatch

```
void         → just call f(args...)
Task<T>      → async path (see §11)
anything else → res.json(f(args...))
```

The auto-serialization via `res.json()` requires that `T` has a `to_json` overload (provided by `OSODIO_SCHEMA`).

### Special argument types

```cpp
template<typename T, fixed_string Name>
struct PathParam {
    T value;
    operator T() const { return value; }  // implicit conversion
};

template<typename T>
struct Body {
    T value;
    const T* operator->() const { return &value; }
    const T& operator*()  const { return value; }
};

template<typename T, fixed_string Name>
struct Query {
    T value;
    operator T() const { return value; }
};
```

`fixed_string<N>` is a C++20 NTTP (non-type template parameter) helper that allows string literals as template arguments (`PathParam<int, "id">`).

---

## 7. Request

**Header**: `include/osodio/request.hpp`

```cpp
struct Request {
    std::string method;    // "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD"
    std::string path;      // "/users/42" (without query string)
    std::string version;   // "HTTP/1.1" or "HTTP/1.0"
    std::string body;      // raw request body

    std::unordered_map<std::string, std::string> headers;  // lowercase keys
    std::unordered_map<std::string, std::string> params;   // path params
    std::unordered_map<std::string, std::string> query;    // query params

    core::EventLoop* loop;  // pointer to event loop; used for co_await sleep()

    // Helpers
    std::optional<std::string> header(std::string name) const;  // case-insensitive lookup
    std::string query_param(const std::string& key, const std::string& def = "") const;
};
```

Headers are stored with lowercase keys (e.g., `"content-type"`, not `"Content-Type"`). Use `req.header("content-type")` for case-insensitive lookup.

`req.loop` is set by the `DispatchFn` before calling any handler. Pass it to `sleep()` when writing async handlers.

---

## 8. Response

**Header**: `include/osodio/response.hpp`

`Response` holds a `shared_ptr<State>` internally so that copies in async captures all share the same underlying data.

```cpp
class Response {
public:
    // Status code (default: 200)
    Response& status(int code);

    // Body setters (set Content-Type automatically)
    Response& json(const nlohmann::json& j);
    Response& html(const std::string& content_or_filename);
    Response& text(std::string body);
    Response& send(std::string body);   // raw body, no Content-Type change

    // Headers
    Response& header(std::string key, std::string value);

    // Introspection
    int status_code() const;
    const std::string& body() const;

    // Async internal API (used by HandlerTraits and HttpConnection)
    bool is_async() const;
    void mark_async();
    void unmark_async();
    void on_complete(std::function<void()> cb);
    void complete_async();

    // Called by App::run to set the templates base directory
    void set_templates_dir(const std::string& dir);

    // Serialize to an HTTP/1.1 response string
    std::string build() const;
};
```

`html(content)` uses a heuristic to decide if `content` is a filename or inline HTML:
- **Filename** if: no `\n`, no `<`, ends with `.html` or `.htm`.
- **Inline HTML** otherwise.

`build()` produces:
```
HTTP/1.1 200 OK\r\n
Content-Length: <n>\r\n
<headers>\r\n
\r\n
<body>
```

---

## 9. JSON Serialization — OSODIO_SCHEMA

**Header**: `include/osodio/schema.hpp`

```cpp
#define OSODIO_SCHEMA(Type, ...) \
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, __VA_ARGS__)
```

`OSODIO_SCHEMA` is a thin wrapper over nlohmann/json's `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`. It must be placed **outside** the struct, in the same namespace.

```cpp
struct Article {
    int         id;
    std::string title;
    std::string body;
    bool        published;
};
OSODIO_SCHEMA(Article, id, title, body, published)
```

This generates:
```cpp
void to_json(nlohmann::json& j, const Article& t) {
    j = nlohmann::json{{"id", t.id}, {"title", t.title}, ...};
}
void from_json(const nlohmann::json& j, Article& t) {
    j.at("id").get_to(t.id);
    j.at("title").get_to(t.title);
    ...
}
```

`from_json` throws `nlohmann::json::out_of_range` if a required field is missing. The `Body<T>` extractor catches this and returns 400.

**Note:** `OSODIO_SCHEMA` and `OSODIO_VALIDATE` are independent and can be combined:

```cpp
struct User {
    std::string name;
    int age;
    OSODIO_VALIDATE(User,
        check(age >= 18, "Must be 18+")
    )
};
OSODIO_SCHEMA(User, name, age)
```

---

## 10. Validation — OSODIO_VALIDATE

**Header**: `include/osodio/schema.hpp`, `include/osodio/validation.hpp`

`OSODIO_VALIDATE` is placed **inside** the struct body. It needs access to `this->` member names.

```cpp
#define OSODIO_VALIDATE(Type, ...)                                    \
    void _validate_impl(std::vector<std::string>& _errors) const {   \
        __VA_ARGS__;                                                  \
    }                                                                 \
    friend void validate(const Type& _self) {                        \
        std::vector<std::string> _errors;                            \
        _self._validate_impl(_errors);                               \
        if (!_errors.empty())                                        \
            throw osodio::ValidationError(std::move(_errors));       \
    }

#define check(cond, msg) \
    ((cond) ? (void)0 : _errors.push_back(msg))
```

The `friend void validate(const Type&)` is found via ADL (Argument-Dependent Lookup) when calling `validate(user)`. The `has_validate<T>` trait in `handler_traits.hpp` detects this at compile time:

```cpp
template<typename T>
struct has_validate<T,
    std::void_t<decltype(validate(std::declval<const T&>()))>
> : std::true_type {};
```

**ValidationError**:
```cpp
struct ValidationError : public std::runtime_error {
    std::vector<std::string> messages;
};
```

**Manual validation**:
```cpp
User u{"", 15};
try {
    validate(u);   // throws ValidationError
} catch (const osodio::ValidationError& e) {
    for (const auto& msg : e.messages) { ... }
}
```

**Validator builders** (from `validation.hpp`):

These are helper lambdas for use with `validate_field()`. They are **not** needed inside `OSODIO_VALIDATE` — use plain `check()` expressions there. They exist as utilities for manual validation code:

```cpp
// min(m) — value must be >= m
// max(m) — value must be <= m
// len_min(m) — string length must be >= m
// len_max(m) — string length must be <= m

std::vector<std::string> errs;
validate_field(user.age,  "age",  errs, min(18), max(99));
validate_field(user.name, "name", errs, len_min(2), len_max(64));
```

---

## 11. Async — Task\<T\> and Coroutines

**Header**: `include/osodio/task.hpp`

### Task\<T\>

```cpp
template<typename T>
struct Task {
    struct promise_type {
        T                      result;
        std::exception_ptr     exception;
        std::function<void(T)> on_complete;    // called at co_return
        std::coroutine_handle<> continuation;  // outer Task waiting on us
        core::EventLoop*        loop = nullptr; // for frame self-destruction
    };

    std::coroutine_handle<promise_type> handle;

    // Transfer ownership to the promise (prevents ~Task from destroying the frame)
    std::coroutine_handle<promise_type> detach();

    bool done() const;
    T    get_result();

    // Awaiter interface — allows co_await Task<T> from another coroutine
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> outer) noexcept;
    T    await_resume();
};
```

`Task<void>` is a separate full specialization with the same structure minus `result` and `on_complete(T)`.

### Coroutine Frame Lifetime

The frame lifecycle in the async path (`HandlerTraits::call`):

```
1. f(args...) creates the coroutine frame and returns Task<T>
2. task.detach() transfers ownership from Task wrapper to the promise
   → ~Task() will not destroy the frame
3. h.promise().loop is set so FinalAwaitable can post h.destroy()
4. h.promise().on_complete is set to: res.json(val); res.complete_async();
5. h.resume() starts the coroutine
6. If the coroutine suspends (e.g. co_await sleep):
   - epoll loop continues normally
   - detached thread fires after `ms` ms, posts h.resume() to the loop
   - loop resumes the coroutine
7. co_return value → promise_type::return_value(val)
   → on_complete(val) is called: res.json(val), res.complete_async()
   → complete_async() calls on_complete_cb (set by http_connection)
   → http_connection posts finish_dispatch to the loop
8. FinalAwaitable::await_suspend runs:
   - no continuation → posts h.destroy() to the loop
9. Loop runs h.destroy() after finish_dispatch → frame freed
```

### SleepAwaitable

```cpp
struct SleepAwaitable {
    int ms;
    core::EventLoop* loop;

    bool await_ready() const noexcept { return ms <= 0; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        std::thread([ms, loop, h]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            loop->post([h]() mutable { if (!h.done()) h.resume(); });
        }).detach();
    }
    void await_resume() const noexcept {}
};

inline SleepAwaitable sleep(int ms, core::EventLoop* loop) { return {ms, loop}; }
```

The detached thread approach is simple but not scalable for high-concurrency timers. For production use, a `timerfd`-based implementation would be more efficient.

### Chaining Tasks

```cpp
Task<int> step_one(core::EventLoop* loop) {
    co_await sleep(10, loop);
    co_return 42;
}

app.get("/chain", [](Request& req) -> Task<nlohmann::json> {
    int v = co_await step_one(req.loop);
    // step_one's FinalAwaitable performs symmetric transfer → resumes this coroutine
    co_return nlohmann::json{{"value", v}};
});
```

Chaining works via `Task::await_suspend`:
1. `await_suspend` stores the outer coroutine handle as `continuation` on the inner task's promise.
2. `FinalAwaitable::await_suspend` checks for `continuation` before posting `h.destroy()`.
3. If `continuation` exists, it performs symmetric transfer: `return continuation;`.
4. The outer coroutine resumes with the inner result via `await_resume`.

---

## 12. Middleware

**Type**: `std::function<void(Request&, Response&, NextFn)>`  
where `NextFn = std::function<void()>`

Middleware is registered with `app.use()` and runs in registration order before every route handler. It does **not** run for static file requests (those are handled before the middleware chain).

```cpp
app.use([](Request& req, Response& res, auto next) {
    // before
    auto start = std::chrono::steady_clock::now();

    next();  // runs the rest of the chain (other middlewares + the route)

    // after (only for synchronous handlers)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << req.path << " " << res.status_code() << " " << ms << "ms\n";
});
```

**Important**: Code after `next()` does **not** run for async handlers because the coroutine suspends during `next()`. Only code before `next()` is reliable in all cases.

**Short-circuiting** (e.g., auth middleware):
```cpp
app.use([](Request& req, Response& res, auto next) {
    if (!req.header("authorization")) {
        res.status(401).json({{"error", "Unauthorized"}});
        return;   // do not call next()
    }
    next();
});
```

**Implementation**: The chain is built using a `shared_ptr<std::function<void(size_t)>>` to avoid dangling references in the `next` closures. The index `i` steps through `middlewares_` and calls the router at `i == middlewares_.size()`.

---

## 13. Static Files

**Implementation**: `src/app.cpp` (`try_serve_static`, `mime_for_ext`)

Static mounts bypass the middleware chain entirely. They are checked at the top of `DispatchFn`, before any middleware runs.

```cpp
app.serve_static("/static", "./public");
// GET /static/style.css  → read ./public/style.css, send with Content-Type: text/css
// GET /static/../../etc/passwd → 403 Forbidden
```

**Multiple mounts** (first match wins):
```cpp
app.serve_static("/assets", "./dist/assets");
app.serve_static("/uploads", "/var/uploads");
```

**Path traversal protection**: `fs::weakly_canonical` is used to resolve symlinks and `..` segments. If the resolved file path is not a child of the resolved root, a 403 is returned.

**Supported MIME types**:

| Extension | MIME type |
|-----------|-----------|
| `.html`, `.htm` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js` | `application/javascript; charset=utf-8` |
| `.json` | `application/json; charset=utf-8` |
| `.svg` | `image/svg+xml` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.webp` | `image/webp` |
| `.ico` | `image/x-icon` |
| `.woff` | `font/woff` |
| `.woff2` | `font/woff2` |
| `.ttf` | `font/ttf` |
| `.pdf` | `application/pdf` |
| `.xml` | `application/xml` |
| `.txt` | `text/plain; charset=utf-8` |
| *(anything else)* | `application/octet-stream` |

---

## 14. Error Handlers

**Type**: `std::function<void(int code, Request&, Response&)>`

Error handlers are called at the end of `DispatchFn`, after the full middleware + route chain, when `res.status_code() >= 400`.

```cpp
app.on_error(404, [](int, Request& req, Response& res) {
    res.status(404).html("404.html");  // must re-set status; handler may change it
});

app.on_error([](int code, Request&, Response& res) {
    res.json({{"error", "An error occurred"}, {"code", code}});
});
```

**Notes**:
- The error handler receives the response object after the route handler has written to it. It can overwrite the body but must call `res.status(code)` again if it wants to change the status.
- Async handlers: error handlers are **not** called for async responses because the response may not be set yet when the check runs. This is a known limitation.
- If no handler is registered for a code and no catch-all is set, the response is sent as-is.

---

## 15. HTML Templates

Templates are loaded from a configurable directory (default: `./templates`).

```cpp
app.set_templates("./views");

app.get("/about", [](Request&, Response& res) {
    res.html("about.html");   // reads ./views/about.html
});
```

`res.html()` accepts either:
- A **filename** (no `<`, no `\n`, ends with `.html` or `.htm`): reads from the templates directory.
- An **inline HTML string**: sent directly.

Currently templates are static files — no variables, no templating engine. Dynamic content must be constructed as a string in the handler.

---

## 16. Event Loop

**Header**: `include/osodio/core/event_loop.hpp`  
**Implementation**: `src/core/event_loop.cpp`

```cpp
class EventLoop {
public:
    using Callback = std::function<void(uint32_t events)>;

    void add   (int fd, uint32_t events, Callback cb);  // register fd
    void modify(int fd, uint32_t events);               // change events
    void remove(int fd);                                 // deregister and remove callback

    void post(std::function<void()> cb);  // thread-safe: schedule on next iteration
    void run();                           // blocks; processes events
    void stop();                          // signals run() to return
};
```

**Internals**:
- Uses `epoll` with `EPOLLET` (edge-triggered) on Linux.
- `eventfd` is used as a wakeup mechanism: `post()` writes to the fd to wake `epoll_wait`.
- `task_queue_` is protected by a mutex; tasks are drained (under lock) then executed (without lock) at the start of each `epoll_wait` iteration.

The event loop runs `process_tasks()` before each `epoll_wait` call, ensuring that tasks posted from coroutines or threads are executed on the loop thread.

---

## 17. HTTP Parser

**Header**: `src/http/http_parser.hpp`  
**Implementation**: `src/http/http_parser.cpp`

An incremental, line-based HTTP/1.1 request parser.

```
States: REQUEST_LINE → HEADERS → BODY (if Content-Length > 0)
```

- Accumulates bytes in a string buffer.
- Scans for `\r\n` lines.
- Parses: method, path (split from query string), HTTP version.
- Headers stored with lowercase keys.
- `Content-Length` header determines body size.
- Automatically resets after emitting a complete request (supports keep-alive pipelining).
- Returns `false` from `feed()` on a parse error; caller closes the connection.

`ParsedRequest` fields:
```cpp
struct ParsedRequest {
    std::string method;
    std::string path;    // without query string
    std::string query;   // raw query string (e.g. "page=1&limit=20")
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};
```

---

## 18. TCP Server

**Header**: `src/core/tcp_server.hpp`  
**Implementation**: `src/core/tcp_server.cpp`

Creates a listening TCP socket and accepts connections.

Socket options set: `SO_REUSEADDR`, `SO_REUSEPORT`, `TCP_NODELAY`, non-blocking (`O_NONBLOCK`).

Address family selection: `AF_INET` for `0.0.0.0` or any IPv4 address, `AF_INET6` for `::` or explicit IPv6 addresses.

On `accept(2)`: creates an `HttpConnection`, registers it with the event loop, stores it in a `shared_ptr` map.

---

## 19. HTTP Connection

**Header**: `src/http/http_connection.hpp`  
**Implementation**: `src/http/http_connection.cpp`

One instance per TCP connection. Inherits `enable_shared_from_this`.

```
on_event(EPOLLIN)  → do_read()  → parser_.feed() → dispatch()
on_event(EPOLLERR) → close()
```

`dispatch()`:
- Builds `Request` and `Response` from `ParsedRequest`.
- Calls `dispatch_fn_(req, res)`.
- If `res.is_async()`: sets `on_complete` callback to post `finish_dispatch` to the loop.
- Otherwise: calls `finish_dispatch` immediately.

`finish_dispatch()`:
- Determines keep-alive from HTTP version and `Connection` header.
- Adds `Connection: keep-alive` or `Connection: close` header.
- Calls `Response::build()` then `send_response()`.
- Closes connection if not keep-alive.

`send_response()` writes in a loop until all bytes are sent (blocking write for simplicity).

---

## 20. How to Extend Osodio

### Add a new argument type

1. Define a new wrapper struct:
   ```cpp
   template<fixed_string Name>
   struct Cookie {
       std::string value;
   };
   ```

2. Add a specialization of `extractor<>` in `handler_traits.hpp`:
   ```cpp
   template<fixed_string Name>
   struct extractor<Cookie<Name>> {
       static Cookie<Name> extract(const Request& req, Response&) {
           // parse req.header("cookie") and extract Name
           return {/* value */};
       }
   };
   ```

3. It works automatically in any handler lambda:
   ```cpp
   app.get("/me", [](Cookie<"session"> session) {
       // session.value
   });
   ```

### Add a new response method

Add a method to `Response` in `include/osodio/response.hpp`:

```cpp
Response& redirect(const std::string& url, int code = 302) {
    state_->status_code = code;
    state_->headers["Location"] = url;
    return *this;
}
```

### Add a new validator

Add a function in `include/osodio/validation.hpp`:

```cpp
inline auto email() {
    return [](const std::string& val, const std::string& field,
              std::vector<std::string>& errs) {
        if (val.find('@') == std::string::npos)
            errs.push_back(field + " must be a valid email");
    };
}
```

Use with `validate_field` in manual validation or directly in `check()`:

```cpp
OSODIO_VALIDATE(User,
    check(name.find('@') == std::string::npos, "Name cannot contain @"),
    check(age >= 18, "Must be 18+")
)
```

### Add a new HTTP method

In `app.hpp`, add a new template method mirroring `get`, `post`, etc.:

```cpp
template<typename F> App& options(std::string path, F&& h) {
    router_.add("OPTIONS", std::move(path), std::forward<F>(h));
    return *this;
}
```

### Add a MIME type for static files

In `src/app.cpp`, `mime_for_ext()` is a simple chain of `if` comparisons. Add a new line:

```cpp
if (ext == ".webmanifest") return "application/manifest+json";
```

### Add a new route parameter type

To support `PathParam<MyType, "name">` where `MyType` is not a built-in:

In the `extractor<PathParam<T, Name>>` specialization in `handler_traits.hpp`, add a branch:

```cpp
else if constexpr (std::is_same_v<T, MyType>)
    return {MyType::from_string(it->second)};
```

---

## Common Patterns

### JSON API with validation

```cpp
struct CreatePostBody {
    std::string title;
    std::string content;
    int         author_id;

    OSODIO_VALIDATE(CreatePostBody,
        check(title.size() >= 5,   "Title must be at least 5 characters"),
        check(title.size() <= 200, "Title too long"),
        check(!content.empty(),    "Content cannot be empty"),
        check(author_id > 0,       "Invalid author_id")
    )
};
OSODIO_SCHEMA(CreatePostBody, title, content, author_id)

app.post("/posts", [](Body<CreatePostBody> body) {
    // body.value is guaranteed valid here
    return nlohmann::json{{"id", 42}, {"title", body->title}};
});
```

### Returning custom structs

```cpp
struct UserResponse {
    int         id;
    std::string name;
    std::string email;
};
OSODIO_SCHEMA(UserResponse, id, name, email)

app.get("/users/:id", [](PathParam<int, "id"> id) -> UserResponse {
    return {id.value, "Alice", "alice@example.com"};
});
```

### Async database simulation

```cpp
Task<nlohmann::json> query_user(int id, core::EventLoop* loop) {
    co_await sleep(10, loop);   // simulate I/O
    co_return nlohmann::json{{"id", id}, {"name", "Alice"}};
}

app.get("/users/:id", [](PathParam<int, "id"> id, Request& req) -> Task<nlohmann::json> {
    auto user = co_await query_user(id.value, req.loop);
    co_return user;
});
```

### CORS middleware

```cpp
app.use([](Request& req, Response& res, auto next) {
    res.header("Access-Control-Allow-Origin", "*");
    res.header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    res.header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    if (req.method == "OPTIONS") {
        res.status(204);
        return;  // don't call next() for preflight
    }
    next();
});
```

### Auth middleware

```cpp
app.use([](Request& req, Response& res, auto next) {
    auto auth = req.header("authorization");
    if (!auth || auth->rfind("Bearer ", 0) != 0) {
        res.status(401).json({{"error", "Unauthorized"}});
        return;
    }
    // optionally attach parsed user to req.params for downstream handlers
    req.params["_token"] = auth->substr(7);
    next();
});
```

### Query parameters

```cpp
app.get("/search", [](Query<std::string, "q"> q, Query<int, "page"> page) {
    return nlohmann::json{
        {"query", q.value},
        {"page",  page.value}
    };
});
// GET /search?q=hello&page=2 → {"query":"hello","page":2}
```

### SPA fallback (send index.html for unknown routes)

```cpp
app.on_error(404, [](int, Request& req, Response& res) {
    // API routes return JSON 404; everything else gets the SPA
    if (req.path.rfind("/api/", 0) == 0) {
        res.status(404).json({{"error", "Not found"}});
    } else {
        res.status(200).html("index.html");
    }
});
```
