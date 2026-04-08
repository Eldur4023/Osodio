# Plan: Osodio — C++20 Web Framework

---

## Estado actual (2026-04-08)

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
| **Middleware** | logger(), cors(), compress() (gzip zlib) |
| **Static files** | MIME, path traversal block, ETag, Cache-Control, 304 Not Modified |
| **Templates** | res.render() vía inja (Jinja2), Environment cacheado por thread×dir |
| **OpenAPI** | DocBuilder\<F\> compile-time, /openapi.json, /docs Swagger UI |
| **DI** | app.provide\<T\>(shared_ptr) y app.provide\<T\>(factory) |
| **Deps vendored** | 8 archivos en third_party/, cero red en cmake |

---

## Roadmap

### Nivel siguiente: "Frontend-ready"

- [ ] **SPA fallback** en serve_static — redirigir rutas desconocidas a /index.html
- [ ] **sendfile()** para archivos estáticos — zero-copy, datos van directo de FS a socket
- [ ] **WebSocket** — upgrade desde HTTP/1.1, framing RFC 6455, pub/sub por topic
- [ ] **Server-Sent Events (SSE)** — `text/event-stream`, backpressure, reconexión automática
- [ ] **Multipart/form-data** — parser incremental para file uploads
- [ ] **Rate limiting** — token bucket por IP, por endpoint; `osodio::rate_limit(opts)`
- [ ] **helmet()** — headers de seguridad: CSP, HSTS, X-Frame-Options, X-Content-Type-Options

### Nivel producción

- [ ] **HTTP/2** con nghttp2 — multiplexing, HPACK, server push
- [ ] **TLS 1.3** con OpenSSL — ALPN para h2, SNI, hot-reload de certificados
- [ ] **io_uring backend** — batch syscalls, reducir overhead vs epoll (Linux 5.1+)
- [ ] **Multi-thread real** — SO_REUSEPORT, un EventLoop por core; actualmente forzado a 1 thread para debugging
- [ ] **Graceful shutdown** — drenar conexiones activas antes de exit
- [ ] **Brotli** — mejor ratio que gzip para texto; negociado vía Accept-Encoding
- [ ] **Métricas** — /metrics Prometheus, /health endpoint

### Nivel ecosistema

- [ ] **HTTP/3 / QUIC** — via quiche (Cloudflare) o ngtcp2
- [ ] **Testing utilities** — mock server, request builder para tests de integración
- [ ] **CLI scaffolding** — `osodio new myapp` genera estructura de proyecto

---

## Arquitectura de archivos actual

```
include/osodio/
  osodio.hpp          — include único
  app.hpp             — App, route registration, group(), provide(), run()
  request.hpp         — Request, headers, query, params, loop, container
  response.hpp        — Response builder (json/html/text/render/send)
  router.hpp          — Radix tree, RouteMatch
  types.hpp           — Middleware, Handler, DispatchFn, ErrorHandler
  task.hpp            — Task<T>, Task<void>, SleepAwaitable, thread_local current_loop
  handler_traits.hpp  — HandlerTraits, extractor<T>, fixed_string, has_to_json
  schema.hpp          — OSODIO_SCHEMA, OSODIO_OPTIONAL, OSODIO_VALIDATE, check()
  validation.hpp      — ValidationError, min/max/len_min/len_max validators
  errors.hpp          — HttpError, not_found(), bad_request(), etc.
  di.hpp              — ServiceContainer, Inject<T>
  middleware.hpp      — logger(), cors(), compress()
  openapi.hpp         — DocBuilder<F>, build_openapi_doc(), swagger_ui_html()
  group.hpp           — RouteGroup (prefix + middleware snapshot)
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
