# Plan: Librería C++ para Servidor Web compatible con Frontend JavaScript

---

## Preámbulo: Estado del Arte y Diagnóstico

### Frameworks C++ existentes y sus fallas

Antes de diseñar la librería, es necesario entender por qué los frameworks C++ existentes no logran la ergonomía de Flask/FastAPI. Se estudiaron 8 frameworks principales:

| Framework | Modelo async | JSON auto | Validación | OpenAPI | DI | Routing | Perf. TechEmpower |
|---|---|---|---|---|---|---|---|
| **Crow** | Bloqueante | No | No | No | No | Macros | Muy alta |
| **Drogon** | Callbacks | No | No | No | No | Macros/clase | #1 general |
| **cpp-httplib** | Bloqueante | No | No | No | No | Regex | Baja (8 threads) |
| **Pistache** | libevent | No | No | No | No | REST router | Buena |
| **Oat++** | Ambos | Sí (DTO) | Sí (DTO) | Sí | No | Macros | Buena |
| **RESTinio** | Asio | No | No | No | No | Express-like | Buena |
| **Boost.Beast** | Manual Asio | No | No | No | No | Ninguno | Excelente |
| **uWebSockets** | epoll/kqueue | No | No | No | No | Simple | #1 plaintext |

#### Crow — Lo bueno y lo malo
```cpp
// Crow: routing con macros, correcto pero verboso
CROW_ROUTE(app, "/users/<int>")
([](int id) {
    crow::json::wvalue x;
    x["id"] = id;          // construcción manual del JSON
    x["name"] = "Juan";    // sin tipado, sin validación
    return x;
});
```
**Falla principal:** I/O bloqueante — cada handler ocupa un thread. Sin HTTP/2. Sin serialización automática. Sin docs.

#### Drogon — El más rápido pero el más difícil
```cpp
// Drogon: controllers con macros y callbacks anidados
class UserCtrl : public drogon::HttpController<UserCtrl> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserCtrl::getUser, "/users/{id}", drogon::Get);
    METHOD_LIST_END

    void getUser(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 int id) {
        // Callback hell: si necesitas consultar la DB y luego responder,
        // anidas otro callback dentro de este
        dbClient_->execSqlAsync(
            "SELECT * FROM users WHERE id=?",
            [cb](const drogon::orm::Result& r) {   // callback anidado
                auto resp = drogon::HttpResponse::newHttpResponse();
                // construir JSON manualmente desde el resultado...
                cb(resp);
            },
            [cb](const drogon::orm::DrogonDbException& e) {
                cb(drogon::HttpResponse::newNotFoundResponse());
            },
            id
        );
    }
};
```
**Falla principal:** Callback hell. Sin serialización automática. La separación de controllers en archivos .cc obligatoria rompe el flujo de desarrollo rápido.

#### Oat++ — El más cercano a FastAPI, pero con macros invasivos
```cpp
// Oat++: tiene OpenAPI automático, pero la API es pesada
#include OATPP_CODEGEN_BEGIN(DTO)
class UserDto : public oatpp::DTO {
    DTO_INIT(UserDto, DTO)
    DTO_FIELD(String, name, "user-name");
    DTO_FIELD(Int32, age);
};
#include OATPP_CODEGEN_END(DTO)

// El endpoint es verboso y requiere mucho boilerplate
ENDPOINT("GET", "/users/{id}", getUser,
         PATH(Int64, userId)) {
    auto user = m_service->getUser(userId);
    return createDtoResponse(Status::CODE_200, user);
}
```
**Falla principal:** Los macros `OATPP_CODEGEN_BEGIN/END` son confusos. El sistema DTO requiere más boilerplate que Python's Pydantic. Sin coroutines reales.

#### La brecha vs Flask/FastAPI
```python
# FastAPI: esto es lo que C++ necesita replicar
from fastapi import FastAPI, Depends
from pydantic import BaseModel

class User(BaseModel):
    name: str
    age: int  # validación automática: debe ser int

app = FastAPI()

@app.get("/users/{user_id}", response_model=User)
async def get_user(user_id: int, db: DB = Depends(get_db)):
    # - Tipado automático de path params (user_id es int)
    # - Serialización automática del objeto retornado
    # - OpenAPI docs generados automáticamente
    # - Inyección de dependencias con Depends()
    # - async/await real
    return await db.get_user(user_id)
```

Lo que hace FastAPI tan fácil de usar y que C++ no tiene:
1. **Reflection en runtime** — Python puede inspeccionar tipos en ejecución. C++ no (hasta C++26).
2. **Decoradores** — `@app.get("/")` es sintaxis nativa. En C++ solo hay macros o registration manual.
3. **Pydantic** — validación y serialización en una sola definición de clase.
4. **`async/await` nativo** — Python lo tiene desde 3.5. C++20 tiene coroutines pero la ergonomía es peor.
5. **Inyección de dependencias** — `Depends()` es mágico. En C++ hay que construir un container explícito.

**La estrategia para esta librería:** usar C++20 para acercarse todo lo posible con `concepts`, `if constexpr`, coroutines, y template metaprogramming declarativo — sin macros.

---

## 1. Capa de Red y I/O

### 1.1 Modelo de concurrencia

El cuello de botella principal de cualquier servidor web es el I/O. Existen tres enfoques:

| Modelo | Descripción | Trade-off |
|---|---|---|
| Thread-per-connection | Un hilo del SO por conexión | Simple, pero escala mal (miles de conexiones = miles de hilos) |
| Thread pool + I/O síncrono | Pool fijo de hilos, cada uno atiende varias conexiones en cola | Mejor que el anterior, pero bloquea el hilo en espera de I/O |
| Event loop + I/O asíncrono | Un solo hilo (o pocos) con `epoll`/`kqueue`/`io_uring` | Alta escala, modelo de Node.js/nginx, más complejo de implementar |

**Decisión:** Event loop con I/O no bloqueante + thread pool para trabajo CPU-intensivo.

**Lección de uWebSockets y Drogon:** el modelo de un event loop por thread (uno por CPU core) con `SO_REUSEPORT` escala mejor que un solo loop compartido, porque elimina contención de mutex en el acceptor. Cada thread tiene su propio loop, sus propias conexiones, y no comparte estado con los demás salvo estructuras explícitamente thread-safe.

```
CPU core 0: EventLoop → [conn1, conn4, conn7, ...]
CPU core 1: EventLoop → [conn2, conn5, conn8, ...]
CPU core 2: EventLoop → [conn3, conn6, conn9, ...]
     ↑ cada loop acepta conexiones independientemente via SO_REUSEPORT
     ↑ el kernel balancea conexiones entrantes entre los loops
```

### 1.2 io_uring (Linux 5.1+)

`epoll` tiene syscall overhead por operación. `io_uring` permite encolar múltiples operaciones I/O en batch, reduciendo el número de syscalls dramáticamente.

```
epoll:    read() → syscall → kernel → resultado → syscall de nuevo para siguiente
io_uring: [read, write, read, accept] → un solo syscall → kernel procesa todos → resultados en batch
```

La abstracción debe soportar ambos backends:
```cpp
// Backend seleccionado en compilación o en runtime según disponibilidad
class IoBackend {
    virtual void submit(IoOp op) = 0;
    virtual void poll(std::span<IoEvent> out) = 0;
};
class EpollBackend  : public IoBackend { ... };
class IoUringBackend: public IoBackend { ... };  // Linux 5.1+
class KqueueBackend : public IoBackend { ... };  // macOS/BSD
```

### 1.3 Abstracción de sockets

```
TcpSocket / UnixSocket
  └── TlsSocket (wraps OpenSSL/BoringSSL)
        └── HttpConnection (HTTP/1.1 state machine)
        └── Http2Connection (nghttp2 session)
              └── Http2Stream (lógica de request/response)
        └── WebSocketConnection (después del upgrade)
```

Necesario:
- `bind()`, `listen()`, `accept()` no bloqueante con `O_NONBLOCK`
- `SO_REUSEPORT` para que múltiples loops acepten en el mismo puerto
- Soporte IPv4 e IPv6 simultáneo (`AF_INET6` con `IPV6_V6ONLY=0`)
- Buffer de lectura por conexión con tamaño configurable (evitar allocs por request)
- `sendfile()` / `splice()` para archivos estáticos (zero-copy: datos van directo de FS a socket sin pasar por userspace)

---

## 2. Protocolo HTTP

### 2.1 HTTP/1.1

Funcionalidades obligatorias:

- **Keep-alive / persistent connections** — los navegadores reutilizan conexiones TCP. Sin esto, cada `fetch()` paga el costo del TCP + TLS handshake.
- **Chunked Transfer Encoding** — necesario para streaming (`ReadableStream` en el cliente, útil para LLM token streaming).
- **100 Continue** — para uploads grandes con `Expect: 100-continue`.
- **Pipelining** — el cliente puede enviar múltiples requests sin esperar respuestas; el servidor debe responder en orden.
- **Host header validation** — rechazar requests sin `Host` en HTTP/1.1.

**Parser:** `llhttp` (el parser de Node.js, escrito en C, mantenido activamente). Es incremental: acepta fragmentos de bytes y emite callbacks cuando parsea cada componente. Esto es crítico para el modelo async.

```cpp
// llhttp callbacks
static int on_url(llhttp_t* p, const char* at, size_t len) { ... }
static int on_header_field(llhttp_t* p, const char* at, size_t len) { ... }
static int on_header_value(llhttp_t* p, const char* at, size_t len) { ... }
static int on_body(llhttp_t* p, const char* at, size_t len) { ... }
static int on_message_complete(llhttp_t* p) { ... }
```

**Lección de cpp-httplib:** no usar I/O bloqueante aunque el API parezca más simple. El blocking model limita la concurrencia al tamaño del thread pool (8 por defecto en cpp-httplib).

### 2.2 HTTP/2

Crítico para rendimiento en frontends modernos:

- **Multiplexing** — múltiples streams sobre una sola conexión TCP. Elimina el Head-of-Line blocking de HTTP/1.1. Un bundle de 100 recursos JS/CSS se beneficia enormemente.
- **HPACK header compression** — los headers de `fetch()` se repiten mucho (`Authorization`, `Content-Type`, `Cookie`); comprimirlos reduce latencia.
- **Server Push** — el servidor puede empujar assets antes de que el navegador los pida (Link: `</app.js>; rel=preload`).
- **Flow control por stream** — necesario para backpressure en streaming responses.
- **ALPN negotiation durante TLS handshake** — el cliente y servidor acuerdan HTTP/2 sin round-trip extra.

**Implementación:** integrar `nghttp2` como biblioteca de framing. nghttp2 maneja la máquina de estados de frames HTTP/2 (HEADERS, DATA, SETTINGS, WINDOW_UPDATE, etc.) y expone callbacks. Nosotros manejamos el I/O subyacente.

```cpp
// nghttp2 opera sobre callbacks, no sobre sockets directamente
nghttp2_session_callbacks_new(&callbacks);
nghttp2_session_callbacks_set_send_callback(callbacks, send_cb);
nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_cb);
nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_cb);
nghttp2_session_server_new(&session, callbacks, user_data);
```

### 2.3 HTTP/3 (QUIC) — Fase futura

Usa UDP. Requiere integrar `quiche` (Cloudflare) o `ngtcp2`. Alta complejidad. Posponer hasta que HTTP/2 esté estable.

---

## 3. TLS / HTTPS

Las APIs modernas de JS (ServiceWorkers, WebCrypto, Geolocation, clipboard) solo funcionan en contextos seguros (`https://` o `localhost`).

- Integrar **OpenSSL 3.x** o **BoringSSL** (fork mantenido por Google, más pequeño, sin APIs legacy).
- Soportar **TLS 1.2** mínimo; **TLS 1.3** recomendado (1-RTT handshake vs 2-RTT, forward secrecy por defecto).
- **SNI (Server Name Indication)** — para servir múltiples dominios desde el mismo proceso con certificados distintos.
- **ALPN (Application-Layer Protocol Negotiation)** — negociar HTTP/2 (`h2`) vs HTTP/1.1 (`http/1.1`) durante el handshake.
- **OCSP Stapling** — el servidor incluye la respuesta OCSP en el handshake; evita que el navegador haga una consulta adicional de revocación.
- **Session resumption** — TLS tickets (stateless) o session IDs (stateful) para reconexiones sin full handshake.
- **Certificate hot-reload** — recargar certificados sin reiniciar el servidor (útil con Let's Encrypt y certbot).

---

## 4. Ergonomía: El Problema Central

Esta es la sección más importante. Los frameworks C++ existentes fallan aquí.

### 4.1 El objetivo: paridad de ergonomía con FastAPI

```cpp
// OBJETIVO: así debería verse un endpoint en Osodio

struct CreateUserBody {
    std::string name;          // required
    int age;                   // required, auto-validado como int
    std::optional<std::string> email;  // opcional
};
OSODIO_SCHEMA(CreateUserBody, name, age, email);  // una sola macro, no CODEGEN_BEGIN/END

app.post("/users", [](CreateUserBody body) -> UserResponse {
    // body ya está deserializado y validado
    // el tipo de retorno define el schema de la respuesta en OpenAPI
    return db.create(body);
});
```

Comparado con FastAPI:
```python
class CreateUserBody(BaseModel):
    name: str
    age: int
    email: Optional[str] = None

@app.post("/users", response_model=UserResponse)
async def create_user(body: CreateUserBody):
    return await db.create(body)
```

La diferencia es pequeña. C++ requiere `OSODIO_SCHEMA` donde Python usa type hints. Todo lo demás puede ser igual de limpio.

### 4.2 Técnica: serialización automática sin reflection

C++ no tiene reflection en runtime (hasta C++26 con P2996). Hay tres técnicas para simularla:

#### Técnica A: Macro de registro (como Oat++, pero menos invasiva)
```cpp
struct User {
    std::string name;
    int age;
    std::optional<std::string> email;
};
// Una sola macro fuera de la definición del struct
OSODIO_SCHEMA(User, name, age, email);
// Genera: to_json, from_json, validate, openapi_schema para User
```
La macro expande a especializaciones de template:
```cpp
template<> struct Schema<User> {
    static constexpr auto fields = std::tuple{
        Field{"name", &User::name},
        Field{"age",  &User::age},
        Field{"email",&User::email}
    };
};
```
Esto permite que `from_json<User>(json_value)` y `to_json(user)` funcionen sin código adicional.

#### Técnica B: Template specialization manual (sin macros, más verboso)
```cpp
template<> struct Schema<User> {
    using type = User;
    static auto fields() {
        return std::tuple{
            field("name", &User::name),
            field("age",  &User::age)
        };
    }
};
```
Más limpio pero requiere más escritura.

#### Técnica C: BOOST_DESCRIBE (C++14+, Boost.Describe)
```cpp
struct User {
    std::string name;
    int age;
};
BOOST_DESCRIBE_STRUCT(User, (), (name, age))
// Boost.Describe genera metadata de compile-time que otros templates pueden usar
```

**Decisión:** Técnica A con la macro `OSODIO_SCHEMA` — mínima fricción, sin dependencia de Boost, funciona en C++20.

### 4.3 Técnica: routing type-safe sin macros de Crow

Crow usa macros porque en C++11/14 no había forma elegante de extraer tipos de una firma de lambda. Con C++20 y `concepts` + deducción de argumentos de templates, se puede hacer sin macros:

```cpp
// Crow (macro obligatoria):
CROW_ROUTE(app, "/users/<int>")([](int id) { ... });

// Osodio (sin macro, usando deducción de templates C++20):
app.get("/users/{id}", [](PathParam<int, "id"> id, const Request& req) {
    // PathParam<T, Name> es un tipo con conversion automática
    // el router infiere que "id" en la ruta debe mapearse a este parámetro
});
```

Internamente, `PathParam<int, "id">` usa `std::string_view` como NTTP (Non-Type Template Parameter, disponible en C++20):
```cpp
template<typename T, std::string_view Name>
struct PathParam {
    T value;
    operator T() const { return value; }
};
```

Para la firma del handler, usamos `requires` de C++20 para validar en tiempo de compilación:
```cpp
template<typename F>
concept Handler = requires(F f, Request& req, Response& res) {
    { f(req, res) };
} || requires(F f) {
    { f() } -> std::convertible_to<Response>;
};
```

### 4.4 Técnica: async/await con coroutines C++20

Drogon usa callbacks. La alternativa elegante es C++20 coroutines:

```cpp
// Drogon (callback hell):
void getUser(const Req& req, std::function<void(Resp)>&& cb, int id) {
    db_->asyncQuery("SELECT...", [cb, id](Result r) {
        cache_->asyncGet(id, [cb, r](CacheResult cr) {
            // 3 niveles de indentación para una operación simple
            cb(buildResponse(r, cr));
        });
    });
}

// Osodio (coroutines):
Task<Response> getUser(int id) {
    auto user = co_await db.query<User>("SELECT...", id);
    auto cached = co_await cache.get(id);
    co_return Response::json(merge(user, cached));
}
```

Para implementar esto se necesita:
1. Un tipo `Task<T>` que sea un awaitable (implementa `promise_type`, `await_suspend`, etc.)
2. Un scheduler que retome las coroutines cuando el I/O completa (integrado con el event loop)
3. Wrappers async para todas las operaciones I/O: `co_await socket.read()`, `co_await db.query()`, etc.

```cpp
// Estructura básica de Task<T>
template<typename T>
struct Task {
    struct promise_type {
        T result_;
        std::coroutine_handle<> continuation_;

        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {continuation_}; }
        void return_value(T v) { result_ = std::move(v); }
        void unhandled_exception() { std::terminate(); }
    };

    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle_.promise().continuation_ = h;
        // Registrar handle_ en el event loop para que se ejecute
        EventLoop::current().schedule(handle_);
    }
    T await_resume() { return std::move(handle_.promise().result_); }

    std::coroutine_handle<promise_type> handle_;
};
```

**Nota:** RESTinio usa Asio que tiene su propia capa de coroutines (`asio::awaitable`). Nosotros implementamos la nuestra para no depender de Boost/Asio.

### 4.5 Técnica: Inyección de dependencias

FastAPI usa `Depends()`. C++ puede hacer algo similar con templates:

```cpp
// FastAPI:
async def get_user(db: DB = Depends(get_db)):
    ...

// Osodio equivalente:
app.get("/users/{id}", [](int id, Inject<Database> db, Inject<Cache> cache) -> Task<User> {
    auto user = co_await db->find(id);
    co_return user;
});
```

`Inject<T>` resuelve el servicio desde un contenedor:
```cpp
template<typename T>
struct Inject {
    T* operator->() { return ServiceLocator::get<T>(); }
    T& operator*() { return *ServiceLocator::get<T>(); }
};

// Registro de servicios en el arranque
app.provide<Database>([]{ return std::make_shared<PostgresDatabase>(conn_str); });
app.provide<Cache>([]{ return std::make_shared<RedisCache>(redis_url); });
```

El router detecta en tiempo de compilación qué parámetros del handler son `Inject<T>` y los resuelve automáticamente:
```cpp
template<typename F, typename... Args>
void dispatch(F& handler, Request& req) {
    // Construir la tupla de argumentos inspeccionando la firma de F
    auto args = build_args<Args...>(req);  // resuelve PathParam, Inject, Body, etc.
    std::apply(handler, args);
}
```

---

## 5. Generación Automática de OpenAPI

Esta es la característica diferenciadora de FastAPI que ningún framework C++ implementa bien excepto Oat++ (y Oat++ lo hace con macros invasivas).

### 5.1 Cómo funciona en FastAPI

FastAPI inspecciona las anotaciones de tipo de cada función en runtime usando `inspect` + `typing`. Construye un objeto `OpenAPI` en memoria y lo sirve como JSON en `/openapi.json`. Swagger UI consume ese JSON.

### 5.2 Cómo hacerlo en C++ sin reflection

En C++, los tipos son información de compile-time. Podemos generar el schema OpenAPI en compile-time y materializarlo como un string JSON en el arranque del servidor.

```cpp
// Cada tipo registrado con OSODIO_SCHEMA tiene un método estático openapi_schema()
template<>
struct Schema<User> {
    static nlohmann::json openapi_schema() {
        return {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}},
                {"age",  {{"type", "integer"}}},
                {"email",{{"type", "string"}, {"nullable", true}}}
            }},
            {"required", {"name", "age"}}
        };
    }
};
```

La macro `OSODIO_SCHEMA` genera este método inspeccionando los tipos de los miembros:
```cpp
// std::optional<T> → {type: T, nullable: true}
// int              → {type: "integer"}
// std::string      → {type: "string"}
// std::vector<T>   → {type: "array", items: Schema<T>::openapi_schema()}
```

Cuando se registra una ruta:
```cpp
app.get("/users/{id}", handler);
```
El router añade al documento OpenAPI:
```json
{
  "paths": {
    "/users/{id}": {
      "get": {
        "parameters": [{"name": "id", "in": "path", "required": true, "schema": {"type": "integer"}}],
        "responses": {"200": {"content": {"application/json": {"schema": {"$ref": "#/components/schemas/User"}}}}}
      }
    }
  }
}
```

### 5.3 Swagger UI integrada

Servir la UI de Swagger desde assets embebidos en el binario (usando `xxd` o `incbin` para convertir los archivos HTML/JS a arrays de bytes en C++):

```cpp
// En el arranque, si swagger está habilitado:
app.get("/docs", [](Request&, Response& res) {
    res.html(SWAGGER_UI_HTML);  // string literal generado en build time
});
app.get("/openapi.json", [&app](Request&, Response& res) {
    res.json(app.openapi_document());
});
```

---

## 6. Sistema de Validación

### 6.1 Validación en la deserialización

Cuando `from_json<User>(json)` falla (campo faltante, tipo incorrecto), debe retornar un error estructurado compatible con el formato de error de HTTP 422 de FastAPI:

```json
{
  "detail": [
    {"loc": ["body", "age"], "msg": "value is not a valid integer", "type": "type_error.integer"},
    {"loc": ["body", "name"], "msg": "field required", "type": "value_error.missing"}
  ]
}
```

```cpp
template<typename T>
std::expected<T, ValidationErrors> from_json(const nlohmann::json& j) {
    ValidationErrors errors;
    T obj;
    // Para cada field en Schema<T>::fields:
    //   si falta en j y no es optional → errors.add(field.name, "field required")
    //   si tipo no coincide → errors.add(field.name, "type error")
    if (!errors.empty()) return std::unexpected(errors);
    return obj;
}
```

### 6.2 Validadores declarativos

```cpp
struct CreateUserBody {
    std::string name;
    int age;
    std::string email;
};
OSODIO_SCHEMA(CreateUserBody, name, age, email,
    // Validadores adicionales (opcionales)
    validate(age, min(0), max(150)),
    validate(email, is_email()),
    validate(name, min_length(1), max_length(100))
);
```

Los validadores son functores que se aplican campo a campo:
```cpp
auto min(int v) {
    return [v](int x) -> std::optional<std::string> {
        if (x < v) return "must be >= " + std::to_string(v);
        return std::nullopt;
    };
}
```

---

## 7. Compatibilidad con JavaScript Frontend

### 7.1 CORS (Cross-Origin Resource Sharing)

El mecanismo más crítico para que `fetch()` funcione desde un origen diferente.

```
Preflight (OPTIONS):
  → Access-Control-Allow-Origin
  → Access-Control-Allow-Methods
  → Access-Control-Allow-Headers
  → Access-Control-Max-Age (cachear el preflight por N segundos)

Solicitud real:
  → Access-Control-Allow-Origin
  → Access-Control-Allow-Credentials: true  (solo si se usan cookies)
  → Vary: Origin  (para que los caches no sirvan la respuesta de un origen a otro)
```

```cpp
app.use(osodio::cors({
    .origins   = {"https://app.ejemplo.com", "http://localhost:5173"},
    .methods   = {Method::GET, Method::POST, Method::PUT, Method::DELETE, Method::PATCH},
    .headers   = {"Content-Type", "Authorization", "X-Request-ID"},
    .max_age   = 86400,
    .credentials = true
}));
```

**Implementación interna:** el middleware CORS maneja automáticamente el preflight `OPTIONS` — no debe llegar al handler del usuario.

### 7.2 WebSocket

```cpp
app.ws("/ws/chat", {
    .on_open = [](WebSocket& ws, const Request& req) -> Task<void> {
        auto user_id = req.cookies["session"];
        ws.data<UserCtx>() = {.user_id = user_id};
        co_await ws.send(json::stringify({{"type", "connected"}}));
    },
    .on_message = [](WebSocket& ws, std::string_view msg, bool is_binary) -> Task<void> {
        // broadcast a todos los subscriptores del mismo canal
        co_await ws.publish("chat", msg);
    },
    .on_close = [](WebSocket& ws, int code, std::string_view reason) {
        // cleanup
    },
    .compression = Compression::Shared,   // permessage-deflate
    .idle_timeout = 60s,
    .max_payload  = 64_KB
});
```

Framing WebSocket a implementar manualmente:
- Opcode: `0x1` (text), `0x2` (binary), `0x8` (close), `0x9` (ping), `0xA` (pong)
- Masking: el cliente DEBE enmascarar, el servidor NO DEBE
- Fragmentation: mensajes grandes se dividen en frames con `FIN=0`, el último con `FIN=1`
- `permessage-deflate` extension para compresión de mensajes

**Lección de uWebSockets:** el pub/sub integrado (como en uWS) es extremadamente útil para casos de uso de chat/live updates. Implementar un topic system similar:
```cpp
ws.subscribe("channel:42");
ws.publish("channel:42", message);   // envía a todos los suscriptores del topic
```

### 7.3 Server-Sent Events (SSE)

Más simple que WebSocket para casos unidireccionales (notificaciones, live logs, token streaming de LLMs):

```cpp
app.get("/events", [](Request& req, SSEStream& stream) -> Task<void> {
    stream.set_retry(3000ms);   // retry automático del cliente si se desconecta
    while (true) {
        auto event = co_await event_bus.next();
        co_await stream.send({
            .event = "update",
            .data  = json::stringify(event),
            .id    = std::to_string(event.id)
        });
    }
});
```

Headers necesarios:
```
Content-Type: text/event-stream
Cache-Control: no-cache
X-Accel-Buffering: no    ← crítico para nginx reverse proxy (deshabilita su buffer)
Connection: keep-alive
```

### 7.4 Compresión

```cpp
// Negociación automática basada en Accept-Encoding del cliente
app.use(osodio::compress({
    .min_size    = 1024,   // no comprimir respuestas < 1KB
    .prefer_br   = true,   // brotli tiene mejor ratio que gzip para texto
    .level_gzip  = 6,      // 1-9, trade-off velocidad/ratio
    .level_br    = 4,      // 0-11
    .skip_types  = {"image/jpeg", "image/png", "image/webp", "video/*"}
}));
```

### 7.5 Caché HTTP

```cpp
// Para archivos estáticos con hash en el nombre (e.g. app.abc123.js):
app.use(osodio::static_files("./dist", {
    .cache_hashed  = "public, max-age=31536000, immutable",  // 1 año
    .cache_default = "public, max-age=3600",
    .etag          = true,
    .spa_fallback  = true   // redirige /* a /index.html para client-side routing
}));
```

---

## 8. Router

### 8.1 Árbol de radix

El router usa un árbol de radix (compressed trie) para matching O(k) donde k = longitud del path (no del número de rutas). Esto es lo que usan nginx, httprouter (Go) y Actix (Rust).

```
Routes:
  GET /api/users
  GET /api/users/:id
  GET /api/users/:id/posts
  POST /api/users

Radix tree:
  /api/
    users           [GET, POST]
      /:id          [GET]
        /posts      [GET]
```

Implementar con los tipos de nodos:
- **Static**: segmento fijo (e.g. `/api/users`)
- **Param**: captura un segmento (e.g. `/:id`)
- **Wildcard**: captura el resto del path (e.g. `/*filepath`)

### 8.2 Grupos y prefijos

```cpp
auto api = app.group("/api/v1");
api.use(authenticate);    // middleware solo para este grupo

auto users = api.group("/users");
users.get("/",    list_users);
users.post("/",   create_user);
users.get("/:id", get_user);
users.put("/:id", update_user);
users.del("/:id", delete_user);
```

### 8.3 Deducción automática de tipos en handlers

El router debe aceptar handlers con cualquiera de estas firmas, detectada en compile-time:

```cpp
// Firma 1: handler síncrono básico
app.get("/ping", [](Request& req, Response& res) { res.text("pong"); });

// Firma 2: handler que retorna un tipo serializable
app.get("/users", []() -> std::vector<User> { return db.all<User>(); });

// Firma 3: handler async con parámetros extraídos
app.get("/users/{id}", [](PathParam<int,"id"> id) -> Task<User> {
    co_return co_await db.find<User>(id);
});

// Firma 4: handler con body deserializado automáticamente
app.post("/users", [](Body<CreateUserDto> body) -> Task<User> {
    co_return co_await db.create<User>(body);
});

// Firma 5: handler con query params
app.get("/search", [](Query<std::string,"q"> q, Query<int,"page",1> page) -> Task<Results> {
    co_return co_await search.query(q, page);
});
```

La magia: `HandlerTraits<F>` analiza la firma de F en compile-time y genera el código de dispatch:
```cpp
template<typename F>
struct HandlerTraits : HandlerTraits<decltype(&F::operator())> {};

template<typename R, typename... Args>
struct HandlerTraits<R(Args...)> {
    using return_type = R;
    using arg_types   = std::tuple<Args...>;
    // Para cada Arg, genera el código para extraerlo del Request
};
```

---

## 9. Middleware

### 9.1 Modelo de cadena

```
Request
  │
  ▼
[Logger] → llama a next()
  │
  ▼
[Auth] → si falla, retorna 401 sin llamar a next()
  │
  ▼
[RateLimit] → si excede, retorna 429
  │
  ▼
[Handler]
  │
  ▼
Response (fluye de vuelta por la cadena en orden inverso)
  │
  ▼
[RateLimit] (post-processing: actualizar contadores)
  │
  ▼
[Logger] (post-processing: registrar status code, duración)
  │
  ▼
Cliente
```

```cpp
// Interfaz de middleware
using NextFn = std::function<Task<void>()>;
using Middleware = std::function<Task<void>(Request&, Response&, NextFn)>;

// Ejemplo de middleware de autenticación
auto authenticate = [](Request& req, Response& res, NextFn next) -> Task<void> {
    auto token = req.header("Authorization");
    if (!token || !jwt::verify(*token)) {
        res.status(401).json({{"error", "unauthorized"}});
        co_return;   // no llama a next() → cortocircuita la cadena
    }
    req.set("user", jwt::decode(*token));
    co_await next();  // llama al siguiente middleware/handler
};
```

### 9.2 Middlewares incluidos en la librería

| Middleware | Descripción | Configurable |
|---|---|---|
| `logger()` | Common Log / JSON estructurado, duración | Formato, destino |
| `cors()` | CORS completo con preflight automático | Origins, methods, headers |
| `compress()` | gzip + brotli negociado por Accept-Encoding | Nivel, tamaño mínimo, tipos excluidos |
| `rate_limit()` | Token bucket por IP, por user, por endpoint | Límites, ventana de tiempo |
| `body_limit()` | Rechaza bodies > N bytes antes de leerlos | Tamaño máximo |
| `json_body()` | Parsea Content-Type: application/json | - |
| `static_files()` | Archivos estáticos con ETag, gzip, SPA fallback | Root, cache policy |
| `helmet()` | Headers de seguridad (CSP, HSTS, X-Frame-Options, etc.) | Cada header configurable |
| `session()` | Sesiones en cookie firmada (HMAC-SHA256) | Secret, TTL, cookie flags |
| `timeout()` | Cancela handlers que excedan N ms | Duración |
| `request_id()` | Genera X-Request-ID si no existe | Header name, generador |

---

## 10. Parsing y Serialización

### 10.1 HTTP Request Parser

`llhttp` (callback-based, incremental) envuelto en una máquina de estados:

```cpp
class HttpParser {
    llhttp_t    parser_;
    llhttp_settings_t settings_;
    ParseState  state_;

    // Buffer management: evitar allocs por cada header
    // Usar un arena allocator para el request actual
    ArenaAllocator arena_;

public:
    // Retorna: bytes consumidos, o error
    std::expected<size_t, ParseError> feed(std::span<const uint8_t> data);
    bool complete() const;
    Request take();  // mueve el request parseado fuera del parser, resetea estado
};
```

**Lección de todos los frameworks:** el parser debe ser zero-copy donde sea posible. Los headers y el path deben ser `std::string_view` apuntando al buffer de lectura, no copias.

### 10.2 JSON

Doble estrategia según el use case:

- **simdjson** para parsing de requests (lectura de JSON entrante): parsing SIMD-acelerado, ~2.5 GB/s en hardware moderno. API de `on-demand` parsing evita construir el DOM completo.
- **nlohmann/json** para generación de responses (construcción de JSON): API ergonómica para construir objetos, no se necesita velocidad extrema en escritura.

```cpp
// Parsing con simdjson (on-demand, zero-copy del buffer)
simdjson::ondemand::parser parser;
auto doc = parser.iterate(body_buffer);
auto name = doc["name"].get_string().value();

// Generación con nlohmann/json
nlohmann::json response = {
    {"id", user.id},
    {"name", user.name},
    {"created_at", format_iso8601(user.created_at)}
};
res.json(response);
```

### 10.3 URL / URI

```cpp
struct ParsedUrl {
    std::string_view scheme;    // "https"
    std::string_view host;      // "api.ejemplo.com"
    uint16_t         port;      // 443
    std::string_view path;      // "/api/users/42"
    QueryParams      query;     // {"page": "1", "limit": "20"}
    std::string_view fragment;  // ""
};

ParsedUrl parse_url(std::string_view url);
std::string url_encode(std::string_view s);
std::string url_decode(std::string_view s);
```

Protección contra path traversal: normalizar el path ANTES de buscar en el filesystem. Rechazar paths que después de normalización queden fuera del root:
```cpp
bool is_safe_path(std::string_view root, std::string_view requested) {
    auto abs = std::filesystem::weakly_canonical(root / requested.substr(1));
    return abs.string().starts_with(root);
}
```

### 10.4 Multipart / Form Data

Necesario para uploads desde `<input type="file">` o `FormData` de JavaScript:

```cpp
app.post("/upload", [](MultipartBody body) -> Task<Response> {
    for (auto& part : body.parts()) {
        if (part.filename()) {
            // guardar archivo
            co_await file.write(part.data());
        }
    }
    co_return Response::json({{"uploaded", body.parts().size()}});
});
```

Parser incremental de multipart (los archivos pueden ser grandes — no leer todo en memoria):
- Detectar boundary del Content-Type header
- Parser de estado: `PREAMBLE → HEADER → BODY → BOUNDARY → ...`
- Soporte para streaming al disco durante el parse (no buffering completo en RAM)

---

## 11. Seguridad

| Vulnerabilidad | Mitigación |
|---|---|
| Path traversal | `weakly_canonical()` + comparar con root antes de abrir |
| HTTP Request Smuggling | Rechazar requests con ambos `Content-Length` y `Transfer-Encoding`; usar llhttp en modo estricto |
| Slowloris | Timeout de headers (ej. 5s), timeout de body (ej. 30s) |
| Large payload DoS | `body_limit()` middleware, configurable por ruta |
| Header injection | `llhttp` valida caracteres; adicionalmente rechazar `\r\n` en valores |
| MIME sniffing | `X-Content-Type-Options: nosniff` en `helmet()` |
| Clickjacking | `X-Frame-Options: DENY` o `frame-ancestors 'none'` en CSP |
| Mixed content / MITM | `Strict-Transport-Security: max-age=63072000; includeSubDomains; preload` |
| XSS vía cookies | `Set-Cookie: ...; HttpOnly; Secure; SameSite=Strict` |
| Directory listing | Deshabilitado por defecto en el static file server |
| Timing attacks en auth | Comparación de tokens con `crypto_memcmp` (tiempo constante) |
| ReDoS | No usar regex para routing (usar radix tree); si se usan regex en validación, limitar complejidad |

---

## 12. Arquitectura de la Librería

### 12.1 Estructura de módulos

```
osodio/
├── CMakeLists.txt
├── include/osodio/
│   ├── osodio.hpp              # include único (convenience header)
│   ├── server.hpp              # App, listen(), group()
│   ├── request.hpp             # Request, Headers, Cookies
│   ├── response.hpp            # Response builder
│   ├── router.hpp              # radix tree + handler dispatch
│   ├── middleware.hpp          # Middleware type + built-in middlewares
│   ├── schema.hpp              # OSODIO_SCHEMA macro + from_json/to_json
│   ├── validation.hpp          # validators: min, max, email, regex, etc.
│   ├── openapi.hpp             # OpenAPI document builder
│   ├── task.hpp                # Task<T> coroutine type
│   ├── websocket.hpp           # WebSocket + pub/sub
│   ├── sse.hpp                 # SSEStream
│   └── inject.hpp              # Inject<T> + service container
├── src/
│   ├── core/
│   │   ├── event_loop.cpp      # epoll/kqueue/io_uring abstraction
│   │   ├── tcp_server.cpp      # socket lifecycle
│   │   └── thread_pool.cpp     # worker threads
│   ├── tls/
│   │   ├── tls_context.cpp     # OpenSSL/BoringSSL wrapper
│   │   └── tls_socket.cpp
│   ├── http/
│   │   ├── http1_parser.cpp    # llhttp wrapper
│   │   ├── http1_conn.cpp      # HTTP/1.1 connection state machine
│   │   ├── http2_conn.cpp      # nghttp2 session wrapper
│   │   └── static_files.cpp    # sendfile, ETag, gzip
│   ├── ws/
│   │   ├── websocket.cpp       # upgrade + framing + pub/sub
│   │   └── sse.cpp
│   └── util/
│       ├── url.cpp             # URL parsing, encoding
│       ├── mime.cpp            # MIME type detection
│       └── base64.cpp          # para Sec-WebSocket-Accept
└── tests/
    ├── unit/
    └── integration/
```

### 12.2 API pública objetivo completa

```cpp
#include <osodio/osodio.hpp>

// --- Definición de schemas ---
struct User {
    int         id;
    std::string name;
    int         age;
    std::optional<std::string> email;
};
OSODIO_SCHEMA(User, id, name, age, email,
    validate(age, min(0), max(150)),
    validate(email, is_email())
);

struct CreateUserBody {
    std::string name;
    int         age;
    std::optional<std::string> email;
};
OSODIO_SCHEMA(CreateUserBody, name, age, email,
    validate(age, min(0), max(150)),
    validate(name, min_length(1), max_length(100))
);

// --- Aplicación ---
int main() {
    osodio::App app;

    // Middleware global
    app.use(osodio::logger());
    app.use(osodio::cors({.origins = {"http://localhost:5173"}}));
    app.use(osodio::compress());
    app.use(osodio::helmet());

    // Servicios inyectables
    app.provide<Database>([] { return std::make_shared<PostgresDB>("..."); });

    // Rutas REST con tipos automáticos
    auto api = app.group("/api/v1");
    api.use(osodio::rate_limit({.per_second = 100}));

    api.get("/users", []() -> Task<std::vector<User>> {
        co_return co_await Inject<Database>{}->find_all<User>();
    });

    api.get("/users/{id}", [](PathParam<int,"id"> id) -> Task<User> {
        auto user = co_await Inject<Database>{}->find<User>(id);
        if (!user) co_return osodio::not_found("user not found");
        co_return *user;
    });

    api.post("/users", [](Body<CreateUserBody> body) -> Task<User> {
        // body ya está validado; si falló validación, se respondió 422 automáticamente
        co_return co_await Inject<Database>{}->create<User>(body);
    });

    // WebSocket con pub/sub
    app.ws("/ws/chat", {
        .on_open    = [](WebSocket& ws) -> Task<void> {
            ws.subscribe("global");
            co_await ws.send(R"({"type":"connected"})");
        },
        .on_message = [](WebSocket& ws, std::string_view msg) -> Task<void> {
            ws.publish("global", msg);
            co_return;
        }
    });

    // SSE para live updates
    app.get("/events", [](SSEStream& stream) -> Task<void> {
        auto& bus = Inject<EventBus>{};
        while (!stream.closed()) {
            auto ev = co_await bus->next();
            co_await stream.send({.event = ev.type, .data = json::stringify(ev)});
        }
    });

    // SPA fallback
    app.use(osodio::static_files("./dist", {.spa_fallback = true}));

    // Documentación automática
    // GET /docs    → Swagger UI
    // GET /openapi.json → spec completa
    app.enable_openapi({.title = "Mi API", .version = "1.0.0"});

    app.listen(8080, [](auto addr) {
        fmt::print("Servidor corriendo en {}\n", addr);
    });
}
```

---

## 13. Dependencias Externas

| Dependencia | Propósito | Por qué esta y no otra | Licencia |
|---|---|---|---|
| `llhttp` | Parser HTTP/1.1 incremental | Es el parser de Node.js; maduro, bien testeado, mantenido activamente | MIT |
| `nghttp2` | Framing HTTP/2 | Estándar de facto para HTTP/2 en C | MIT |
| `OpenSSL 3.x` | TLS | Omnipresente, bien auditado; alternativa: BoringSSL si se quiere menos superficie | OpenSSL |
| `simdjson` | Parsing JSON de requests | ~2.5 GB/s con SIMD; on-demand API evita construir el DOM completo | Apache 2.0 |
| `nlohmann/json` | Generación JSON de responses | API ergonómica; velocidad no crítica en escritura | MIT |
| `zlib` | Compresión gzip | Disponible en todos los sistemas; stable API | zlib |
| `brotli` | Compresión brotli | Mejor ratio que gzip para texto/HTML/JS | MIT |
| `{fmt}` | Formateo de strings, logging | Más rápido que `std::format`; backport para C++17 | MIT |

**No incluir:** ORM (demasiado opinionado), framework de testing (dejar al usuario), logger externo (implementar el propio).

**Gestión de dependencias:** CMake `FetchContent` para reproducibilidad. Soporte opcional de `vcpkg` y `Conan` para usuarios que lo prefieran.

---

## 14. Fases de Implementación

### Fase 1 — Core HTTP (MVP)
- [ ] EventLoop con `epoll` (Linux) / `kqueue` (macOS)
- [ ] TCP server no bloqueante con `SO_REUSEPORT`
- [ ] HTTP/1.1 parser con `llhttp` (incremental, zero-copy)
- [ ] Router radix tree con PathParam
- [ ] Response builder (status, headers, body)
- [ ] `Task<T>` coroutine type básico
- [ ] Middleware chain con `co_await next()`
- [ ] Logger y CORS middleware
- [ ] JSON response helpers con nlohmann/json

### Fase 2 — Ergonomía (FastAPI parity)
- [ ] Macro `OSODIO_SCHEMA` + from_json/to_json generado
- [ ] Validadores declarativos
- [ ] Deducción de tipos en handlers (PathParam, Body, Query, Inject)
- [ ] Generación de OpenAPI 3.0 en compile-time
- [ ] Swagger UI embebida (assets compilados en el binario)
- [ ] HTTP 422 con errores de validación estructurados
- [ ] Service container para inyección de dependencias
- [ ] Respuestas de error tipadas (`not_found()`, `bad_request()`, etc.)

### Fase 3 — Frontend-ready
- [ ] HTTPS con TLS 1.3 (OpenSSL)
- [ ] WebSocket con pub/sub integrado
- [ ] Server-Sent Events
- [ ] Compresión gzip/brotli automática
- [ ] Static file server con ETag, `sendfile()`, SPA fallback
- [ ] Body parser (JSON, multipart/form-data, urlencoded)
- [ ] Rate limiting middleware (token bucket)
- [ ] `helmet()` middleware (security headers)

### Fase 4 — Producción
- [ ] HTTP/2 con `nghttp2`
- [ ] `io_uring` backend (Linux 5.1+)
- [ ] Thread pool configurable (N loops, uno por core)
- [ ] Graceful shutdown (drenar conexiones activas)
- [ ] Hot reload de certificados TLS
- [ ] Métricas Prometheus (`/metrics` endpoint)
- [ ] Health check endpoint (`/health`)
- [ ] Request timeout middleware

### Fase 5 — Ecosistema
- [ ] CLI `osodio-ctl` para scaffolding de proyectos
- [ ] HTTP/3 con QUIC (`quiche` de Cloudflare)
- [ ] Bindings Python (pybind11) para scripts de admin
- [ ] Testing utilities (mock server, request builder)
- [ ] Documentación interactiva al estilo de FastAPI

---

## 15. Consideraciones de Diseño C++

### Estándar: C++20 obligatorio

Se requieren estas features de C++20:
- **Coroutines** (`co_await`, `co_return`) para el modelo async
- **Concepts** para restricciones de tipos en handlers y schemas
- **NTTP strings** (`template<std::string_view Name>`) para `PathParam<T, "id">`
- **`std::span`** para buffers zero-copy
- **Designated initializers** para la config de middlewares (`.origins = {"..."}`)
- **`std::expected<T,E>`** para manejo de errores sin excepciones (disponible C++23, backporteable)

### Coroutines: implementar el scheduler propio, no depender de Asio

RESTinio depende de Boost.Asio para su async. Esto añade una dependencia grande. La estrategia correcta:
1. Implementar `Task<T>` con coroutines C++20 nativas.
2. El event loop integra el scheduler: cuando una coroutine hace `co_await socket.read()`, suspende y el loop la reanuda cuando el fd está listo.
3. Esto elimina la dependencia de Asio y da control total sobre el scheduling.

### Zero-copy

- **Archivos estáticos:** `sendfile()` (Linux) / `sendfile()` (macOS) — los datos van de FS a socket sin pasar por userspace.
- **Headers de request:** `std::string_view` apuntando al buffer de lectura, no copias. El buffer vive hasta que el handler termina.
- **Response body:** scatter-gather I/O con `writev()` para enviar headers y body en un solo syscall sin concatenarlos en memoria.

### Manejo de errores: no excepciones en el hot path

```cpp
// NO: lanzar excepción en el path del servidor (costo de unwinding)
User parse_user(const json& j) {
    if (!j.contains("name")) throw ValidationError("name required");
}

// SÍ: std::expected para errores esperables
std::expected<User, ValidationError> parse_user(const json& j) {
    if (!j.contains("name")) return std::unexpected(ValidationError{"name required"});
    return User{j["name"]};
}
```

Las excepciones se reservan para errores de programación (bugs), no para condiciones esperadas (input inválido, recurso no encontrado).

### Tiempos de compilación

El mayor dolor de cabeza de usar templates agresivamente en C++ son los tiempos de compilación. Estrategias:
- **Precompiled headers (PCH):** los headers de nlohmann/json y simdjson son lentos de parsear; compilarlos una sola vez.
- **Explicit template instantiation:** instanciar `Schema<User>` en un .cpp separado, no en cada translation unit.
- **Módulos C++20:** a largo plazo, migrar los headers internos a módulos para eliminación de includes redundantes.
- **Unity builds:** en CI, compilar todos los .cpp juntos en una sola translation unit reduce el tiempo total.

### Sanitizers y fuzzing en CI

```cmake
# Build de desarrollo: sanitizers
target_compile_options(osodio-tests PRIVATE
    -fsanitize=address,undefined,thread
    -fno-omit-frame-pointer
)

# Build de fuzzing para el parser HTTP (componente más expuesto)
add_executable(fuzz-http-parser fuzz/http_parser_fuzz.cpp)
target_compile_options(fuzz-http-parser PRIVATE -fsanitize=fuzzer,address)
```

El parser HTTP es la superficie de ataque más grande (recibe input arbitrario de Internet). Fuzzear con libFuzzer es no negociable antes del primer release.
