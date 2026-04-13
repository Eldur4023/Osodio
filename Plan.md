# Plan: Osodio — C++20 Web Framework

---

## Estado actual (2026-04-09)

### Implementado y funcionando

| Área | Detalle |
|------|---------|
| **Event loop** | epoll + timerfd + eventfd wakeup + task queue (`src/core/event_loop.cpp`) |
| **TCP server** | SO_REUSEPORT, IPv4/IPv6, SOCK_NONBLOCK, TCP_NODELAY, límite de conexiones atómico |
| **HTTP/1.1 parser** | llhttp incremental, chunked encoding, smuggling protection, keep-alive |
| **Non-blocking I/O** | write_buf_ + EPOLLOUT backpressure, offset sin substr copy |
| **Timeouts** | 5s header (Slowloris), 30s handler+write; timerfd con weak_ptr |
| **Connection limit** | atomic counter en TcpServer; 503 instantáneo al superar max |
| **Router** | Radix tree, nodos STATIC/PARAM/WILDCARD, :param y {param} |
| **Route groups** | app.group(prefix).use(mw) — herencia de middleware, grupos anidados |
| **HandlerTraits** | Deducción de firma en compile-time: extrae y convierte todos los params |
| **OSODIO_SCHEMA** | NLOHMANN_DEFINE_TYPE_INTRUSIVE, _schema_fields(), dentro del struct |
| **OSODIO_OPTIONAL** | Campos ausentes → nullopt; adl_serializer\<optional\<T\>\> incluido |
| **OSODIO_VALIDATE** | _validate_impl(), check() macro, 422 automático |
| **PathParam\<T,"n"\>** | int, long, float, double, string |
| **Query\<T,"n","default"\>** | Default value como tercer fixed_string; operator bool indica presencia |
| **Body\<T\>** | Wrapper explícito con operator bool para comprobar validez |
| **Auto-body** | Cualquier struct con OSODIO_SCHEMA como parámetro bare |
| **Inject\<T\>** | ServiceContainer: singleton + transient; resolve desde req.container |
| **Task\<T\>** | Coroutine type con symmetric transfer, frame autodestruction vía loop |
| **sleep(ms)** | thread_local current_loop + current_token; no requiere pasar req.loop |
| **CancellationToken** | Compartido entre conexión y Request; cancelado en close(); sleep() lo honra |
| **write_buf_ limit** | Hard cap 16 MB por respuesta; cierre limpio si se supera |
| **Header timeout per-keepalive** | Timer de 5s rearmado tras cada respuesta en conexiones keep-alive |
| **Errores tipados** | HttpError, not_found(), bad_request(), unauthorized(), etc. |
| **Middleware** | logger(), cors(), compress() (Brotli preferido, gzip fallback), helmet(), rate_limit() |
| **JWT** | jwt_auth() HS256/RS256; jwt::sign/verify/decode; claims en req.jwt_claims; skip predicate; jwt_auth_rsa() |
| **helmet()** | CSP, HSTS, X-Frame-Options, X-Content-Type-Options, Referrer-Policy; headers pre-built al registrar |
| **rate_limit()** | Fixed-window por IP (o clave custom); headers X-RateLimit-Limit/Remaining; state por instancia |
| **Static files** | MIME, path traversal block, ETag, Cache-Control, 304, sendfile(2) zero-copy |
| **SPA fallback** | serve_static("/", "./dist", true) — rutas sin archivo → index.html con 200 |
| **SSE** | make_sse(res, req) — text/event-stream, named events, ping, is_open() via CancellationToken; HTTP/2: nghttp2 DATA frames via H2SSEContext |
| **Multipart** | parse_multipart(req) — boundary extraction, headers por parte, filename, content_type |
| **remote_ip** | req.remote_ip — IPv4/IPv6 via getpeername() en dispatch() |
| **sleep() early wake** | CancellationToken.set_wake() — cancel() cancela el timerfd y reanuda el coroutine inmediatamente |
| **Templates** | res.render() vía inja (Jinja2), Environment cacheado por thread×dir |
| **OpenAPI** | DocBuilder\<F\> compile-time, opt-in con app.enable_docs() |
| **DI** | app.provide\<T\>(shared_ptr) y app.provide\<T\>(factory) |
| **WebSocket** | RFC 6455: SHA-1/base64 handshake, framing (text/binary/ping/pong/close), fragmentación; HTTP/2: RFC 8441 CONNECT+websocket, nghttp2 DATA frames bidireccionales |
| **Multi-thread** | hardware_concurrency() threads, SO_REUSEPORT, conn_count compartido |
| **Graceful shutdown** | SIGTERM → stop_accepting() + drain poll 100ms, 30s timeout; 2º SIGINT → force exit |
| **Metrics + Health** | app.enable_metrics() → GET /metrics Prometheus; app.enable_health() → GET /health JSON |
| **TLS 1.3** | app.tls("cert.pem","key.pem").run(443); handshake async no-blocking; sendfile→read para HTTPS |
| **HTTP/2** | ALPN (h2/http1.1); Http2Connection + nghttp2; streams concurrentes; HPACK; BodySrc owned por stream |
| **Deps vendored** | 8 archivos en third_party/, cero red en cmake |

---

## Roadmap

### Nivel siguiente: "Producción"

### Nivel producción

- [x] **HTTP/2** con nghttp2 — ALPN, streams concurrentes, HPACK, flujo completo sin server push
- [x] **TLS 1.3** con OpenSSL — app.tls(cert, key).run(443); handshake async + epoll; sendfile fallback para HTTPS
- [x] **io_uring backend** — IORING_POLL_ADD_MULTI, token-based, raw syscalls; `-DUSE_IO_URING=ON`
- [x] **Brotli** — mejor ratio que gzip para texto; negociado vía Accept-Encoding; gzip fallback
- [x] **Métricas** — /metrics Prometheus, /health endpoint

### Nivel ecosistema

- [ ] **HTTP/3 / QUIC** — via quiche (Cloudflare) o ngtcp2
- [x] **TestClient** — `osodio::TestClient` in-process, no socket; builder API `.get/.post/.json/.query/.header/.send()`; sleep() no-op; sendfile reads into body
- [ ] **CLI scaffolding** — `osodio new myapp` genera estructura de proyecto

---

## Arquitectura de archivos actual

```
include/osodio/
  osodio.hpp          — include único
  app.hpp             — App, route registration, group(), provide(), run()
  request.hpp         — Request, headers, query, params, loop, container, remote_ip
  response.hpp        — Response builder (json/html/text/render/send/send_file)
  router.hpp          — Radix tree, RouteMatch
  types.hpp           — Middleware, Handler, DispatchFn, ErrorHandler
  task.hpp            — Task<T>, Task<void>, SleepAwaitable, set_wake early cancel
  cancel.hpp          — CancellationToken con set_wake() para early-wake de sleep()
  handler_traits.hpp  — HandlerTraits, extractor<T>, fixed_string, has_to_json
  schema.hpp          — OSODIO_SCHEMA, OSODIO_OPTIONAL, OSODIO_VALIDATE, check()
  validation.hpp      — ValidationError, min/max/len_min/len_max validators
  errors.hpp          — HttpError, not_found(), bad_request(), etc.
  di.hpp              — ServiceContainer, Inject<T>
  middleware.hpp      — logger(), cors(), compress(), helmet(), rate_limit()
  sse.hpp             — SSEWriter, make_sse(res, req)
  multipart.hpp       — MultipartPart, parse_multipart(req)
  websocket.hpp       — WSMessage, WSConnection, detail::WSState, SHA-1, base64, frame builder
  openapi.hpp         — DocBuilder<F>, build_openapi_doc(), swagger_ui_html()
  metrics.hpp         — Metrics singleton; atomics para requests/conns/uptime; Prometheus + JSON
  group.hpp           — RouteGroup (prefix + middleware snapshot)
  testing.hpp         — TestClient: in-process request builder, sin socket, sync execution
  core/event_loop.hpp — EventLoop interface

src/
  app.cpp             — App::run(), dispatch lambda, static file serving
  router.cpp          — Radix tree implementation
  core/
    event_loop.cpp    — epoll, timerfd, eventfd, post(), schedule_timer()
    tcp_server.cpp    — accept loop, connection limit, SO_REUSEPORT
  http/
    http_parser.hpp/cpp  — llhttp wrapper, ParsedRequest
    http_connection.hpp/cpp — per-connection state, timeouts, write buffer

third_party/          — 8 archivos, cero red en cmake
  nlohmann/json.hpp   — v3.11.3
  simdjson.h/.cpp     — v3.10.0, amalgamated
  inja.hpp            — v3.4.0, single-include
  llhttp/             — v9.2.1 (3 .c + 1 .h)
```

---

## Decisiones de diseño clave

**Por qué epoll y no io_uring ahora:** io_uring tiene menos overhead pero requiere kernel 5.1+ y la API es más compleja. epoll es sólido y está en todos los kernels Linux relevantes. Se puede añadir io_uring como backend alternativo después sin cambiar la interfaz.

**Por qué un solo event loop en debug:** Multi-thread con SO_REUSEPORT es la arquitectura objetivo, pero tiene bugs sutiles de thread-safety que son difíciles de reproducir. Se fuerza `num_threads = 1` hasta que el código sea estable.

**Por qué no Boost.Asio:** Introduce 200+ MB de headers, su modelo de cancelación es complejo, y nuestro event loop es más simple de entender y depurar. El objetivo es que cualquier programador C++ pueda leer `event_loop.cpp` y entender cómo funciona.

**Por qué fixed_string como NTTP:** C++20 permite strings literales como parámetros de template. `PathParam<int, "id">` es más legible que `PathParam<int, id_tag>` y no requiere declarar un tag type para cada parámetro.

**Por qué OSODIO_SCHEMA dentro del struct:** `NLOHMANN_DEFINE_TYPE_INTRUSIVE` genera friend functions que tienen acceso a miembros privados. Además evita tener que repetir el nombre del tipo fuera. El struct queda autocontenido.
