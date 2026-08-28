# Osodio 2.0 — Definición

> Osodio deja de ser un framework de C++ y pasa a ser un **binario único que lee ficheros
> `.odio` y sirve**. El motor nativo no cambia: lo que cambia es que la declaración de
> endpoints se hace en un lenguaje propio, y Osodio traduce esa declaración a llamadas a
> sus propias herramientas.

---

## 1. Los tres problemas que resuelve

1. **Compilar para cambiar un endpoint.** El ciclo de desarrollo pasa a ser guardar el
   fichero y recargar. Sin CMake, sin toolchain, sin C++.
2. **El framework intenta hacer de todo.** TLS, HTTP/2, compresión, rate limiting y
   cabeceras de seguridad se delegan al reverse proxy, que ya los hace mejor.
3. **Sigue siendo verboso frente a FastAPI.** Un lenguaje declarativo elimina la ceremonia
   de plantillas, macros y firmas de lambda.

**Encuadre:** Osodio 2.0 corre **detrás de un reverse proxy** (nginx, Caddy). El proxy es
dueño del transporte; Osodio es dueño de la aplicación.

---

## 2. Modelo de ejecución

```
osodio ./app          →  lee *.odio  →  lex → parse → check → emitir
                         ↓
                      tabla de rutas + bytecode  (construido UNA vez)
                         ↓
                      N hilos, cada uno: event loop + VM propio, SO_REUSEPORT
```

**Compilación única.** Lexer, parser, análisis semántico y emisión ocurren una vez al
arrancar y una vez por cada cambio de fichero. Nunca por request.

**Dos niveles de ruta:**

- **Declarativa** (`get "/" -> render "index.html"`) → compila a una entrada en la tabla de
  rutas con una acción nativa. **Cero pasos interpretados en runtime.**
- **Con lógica** → ejecuta bytecode que solo hace pegamento: resolver nombres y llamar a
  builtins nativos. El trabajo real (parseo HTTP, routing, I/O de fichero, plantillas, JSON)
  siempre es C++ nativo.

**Bytecode, no árbol.** El motivo decisivo no es la velocidad: un intérprete que recursa
sobre la pila de C++ **no puede suspenderse**. Con pila propia sí, y sin eso no hay `await`
en Odio — un handler que toque red o disco bloquearía todas las conexiones de su core.

**Un VM por hilo.** Heap y estado estrictamente por hilo. Nada mutable compartido entre
VMs: compartir heap entre los N loops exigiría un lock global, que es reinventar el GIL.
El estado compartido lo da el lado nativo (§4, `state`).

**Recarga en caliente.** Al detectar un cambio: parsear y validar el fichero nuevo; si
compila, swap atómico del módulo (un `shared_ptr`, sin `dlopen` ni `.so`); si no compila,
se sigue sirviendo el módulo anterior y se imprime el error. **Un typo nunca tumba el
servidor.**

---

## 3. Las tres capas

| Capa | Qué es | Estado |
|---|---|---|
| **1. Motor** | event loop, HTTP/1.1, router, corrutinas, ficheros, plantillas | Ya escrito |
| **2. Herramientas** | Capacidades del motor expuestas como builtins invocables | Ya escrito, hay que exponerlo |
| **3. Frontend Odio** | lexer, parser, check semántico, emisor, watcher, hot swap | **Nuevo** |

El lenguaje solo puede declarar lo que existe en la capa 2. Por eso la capa 2 se cierra
antes de diseñar la sintaxis.

---

## 4. Capa 2 — Superficie de herramientas

Esto es el contrato: todo lo que Odio tiene que saber expresar, y nada más.

### Rutas
- Métodos `GET` `POST` `PUT` `PATCH` `DELETE` `ANY`
- Patrones `:param`, `{param}`, `*`
- Grupos con prefijo y anidamiento

### Entrada
- Parámetros de ruta tipados — `int` `long` `float` `double` `bool` `string`
- Parámetros de query tipados, con valor por defecto y detección de presencia
- Cuerpo JSON mapeado a un struct declarado en Odio
- Cabeceras (lookup case-insensitive)
- Cookies
- Formulario `urlencoded`
- Multipart: ficheros y campos, con `filename` y `content_type` por parte

### Salida
- `json` · `html` (inline o fichero) · `text` · `send` crudo
- `render(plantilla, datos)` — Jinja2 vía **Jinja2Cpp** (ver §9)
- `send_file` — zero-copy `sendfile(2)`
- `status`, cabeceras, cookies (`Secure`, `HttpOnly`, `SameSite`, `Max-Age`)
- Redirección

### Errores
- Tipados: 400, 401, 403, 404, 409, 422, 429, 500
- Handlers de error por código y global

### Validación
- Reglas por campo a nivel de lenguaje → 422 automático, handler no se ejecuta

### Async
- `await` sobre builtins de I/O
- `sleep(ms)` — despierta antes si la conexión se cancela
- Cancelación observable (`is_cancelled`)

### Tiempo real
- **SSE** — `send`, `send_event`, `ping`, `is_open`, reconexión con `Last-Event-ID`
- **WebSocket** — RFC 6455: texto/binario, ping/pong, fragmentación, `allowed_origins`

### Ficheros estáticos
- Montaje `(prefijo, directorio, spa)`
- MIME, ETag, `Cache-Control`, 304, `sendfile(2)`, fallback SPA
- **Bloqueo de dotfiles y defensa contra path traversal** (no negociable)

### Auth
- `jwt.sign` / `jwt.verify` — **HS256 con HMAC-SHA256 vendorizado, sin OpenSSL**
- Protección de rutas y grupos

### Estado compartido
- Store nativo por proceso, respaldado por atómicos/mutex, visible desde todos los VMs.
  Es la única vía de estado compartido entre hilos.

### Observabilidad
- Logger con niveles, ficheros rotados e informe de rendimiento
- `/health`, `/metrics` (Prometheus)
- `/docs` + `/openapi.json` **generados desde el AST**

### Fuera: persistencia
2.0 **no tiene acceso a datos**. Atar el núcleo a un motor de base de datos síncrono
contradice el argumento de eficiencia del proyecto. La persistencia llegará como **módulos**
(§10), no como builtin.

Sin DB, lo que el VM realmente suspende en 2.0 es `ws.recv()` (bloquea hasta que llega un
mensaje) y `sleep()` (bucles de keepalive de SSE). Ambos están en esta superficie, así que
la pila propia sigue siendo obligatoria.

---

## 5. El lenguaje Odio

Gramática formal completa en [ODIO-GRAMMAR.md](ODIO-GRAMMAR.md). Ejemplos trabajados en
`prueba-revisada.odio`, `prueba-avanzada.odio` y `prueba-app.odio`.

Odio es un híbrido deliberado: **bloques por indentación al estilo Python, tipado estático
al estilo C++**. La forma de declarar rutas viene de Flask; la de recibir datos, de FastAPI.

### Decisiones de diseño

**Todo entra por la firma.** Parámetros de ruta, query, body y ficheros son parámetros
tipados del endpoint, no cosas que se extraen a mano del request. El checker verifica que
cada `:nombre` del patrón tiene quien lo recoja, y al revés.

**Todo sale por `return`.** No hay objeto `response` mutable que arrastrar por el handler.
El código, las cabeceras y las cookies se encadenan sobre lo devuelto.

**`require X else Y` sustituye al middleware entero.** Tras delegar CORS, compresión, rate
limiting y cabeceras al proxy, el único middleware que quedaba era proteger rutas. Y eso es
azúcar de `if not X: return Y` — no un constructo aparte.

**Objetos reservados en vez de construcción explícita.** `request`, `session`, `state`,
`jwt`, `sse`, `ws`, `error`, `log`. Existen dentro del contexto que los define, sin pedirlos.

**Genéricos borrados, solo en contenedores nativos.** `List<T>` y `Dict<K,V>` sí; clases
genéricas de usuario no en 2.0. Se comprueban en compilación y desaparecen antes del
bytecode: cuestan complejidad de compilador y **cero en runtime**.

**Tres formas de datos, sin `Any`.** `class` para forma conocida y validada, `Json` para lo
dinámico (body sin esquema, mensaje WS, literal de respuesta), contenedores para lo
homogéneo. Un tipo `Any` envenenaría el sistema entero.

**La configuración es Odio.** Sin YAML ni TOML: un lenguaje que aprender, no dos, y un
puerto mal escrito es error de compilación. Bloque `app:`, una sola vez en el proyecto.

**`session` es cookie firmada** con el HMAC-SHA256 vendorizado. Sin estado en servidor, así
que encaja con un VM por hilo sin nada que sincronizar.

**`state` expone operaciones atómicas, no propiedades**, porque es la única vía de estado
compartido entre VMs y `state.x = state.x + 1` sería una carrera.

### Layout de proyecto

```
mi-app/
  app.odio           configuración
  rutas/*.odio       el orden entre ficheros es indiferente
  templates/         plantillas Jinja2
  public/            estáticos
```

El argumento decide el alcance, sin sorpresas:

| Invocación | Qué compila |
|---|---|
| `osodio app.odio` | Solo ese fichero |
| `osodio rutas/api.odio rutas/admin.odio` | Solo esos dos |
| `osodio ./mi-app` | Todos los `.odio` del directorio, recursivamente |

El orden no importa en ningún caso, porque la compilación es en dos pasadas: primero se
recogen las declaraciones, después se resuelven los nombres. Y el watcher vigila exactamente
el mismo conjunto que se compiló, ni más ni menos.

---

## 6. Qué se corta

| Se corta | Motivo |
|---|---|
| TLS / OpenSSL | Lo hace el reverse proxy |
| HTTP/2 / nghttp2 | Ídem |
| `cors()`, `compress()`, `helmet()`, `rate_limit()` | Ídem |
| `csrf.hpp` | Depende de OpenSSL; el patrón se resuelve en el proxy o en Odio |
| `handler_traits.hpp` (448 líneas) | Existe solo para extraer argumentos de lambdas C++ |
| `schema.hpp` — macro `SCHEMA` | El lenguaje declara los structs |
| `validation.hpp` | Validación a nivel de lenguaje |
| `di.hpp` / `Inject<T>` | No hay código C++ de usuario donde inyectar |
| `group.hpp` | Anidamiento a nivel de lenguaje |
| `testing.hpp` — `TestClient` | Depende de la historia de tests de Odio (sin decidir) |
| `io_uring_loop.cpp` | Segundo backend en un 2.0 que quiere adelgazar. Puede volver |
| `openapi.hpp` (347 líneas) | **Se reescribe** desde el AST, no se conserva |

**~1.400 líneas de metaprogramación de plantillas desaparecen.** `handler_traits`,
`schema`, `validation` y `di` existen únicamente porque el usuario escribe C++. Sin usuario
de C++, el binding de argumentos lo hace el frontend al leer el `.odio`, una vez, sin
plantillas.

### Cirugía necesaria, no borrado limpio

- **nghttp2 se filtra** a `app.hpp`, `request.hpp`, `sse.hpp` y `websocket.hpp`. Cortar
  HTTP/2 toca esas cuatro cabeceras además de `http2_connection.*`.
- **OpenSSL se filtra** a `src/app.cpp`, `src/core/tcp_server.hpp` y
  `src/http/http_connection.*` (ramas TLS del ciclo de lectura/escritura).
- `jwt.hpp` y `csrf.hpp` están enteros dentro de `#ifdef OSODIO_HAS_TLS`: al cortar TLS
  mueren solos. JWT se reimplementa sobre HMAC-SHA256 vendorizado.

---

## 7. Qué hay que construir

1. **Frontend Odio** — lexer, parser, análisis semántico, emisor de bytecode.
   *Hecho en el hito 1:* lexer con INDENT/DEDENT, parser completo de expresiones y
   sentencias, rutas y bloque `app:`. Falta `class`, `fn`, `group`, `on error`, `import`.
2. **VM** — pila propia, suspendible en `await`, una instancia por hilo.
   *Hecho en el hito 2:* pila y locales propios, aritmetica, comparaciones,
   cortocircuito, `if`/`while`/`require`, ternario, listas y diccionarios, llamada
   a builtins, tope de pasos contra bucles infinitos, y errores de ejecucion con
   `fichero:linea:columna`. Tambien `class` con campos, campos opcionales y bloque
   `validate:` compilado a bytecode, con el cuerpo de la peticion enlazado como
   parametro tipado y 422 automatico. Y **`await`**: el VM se detiene y se reanuda
   conservando pila y locales, mientras el `co_await` real lo hace el handler en
   C++. Falta `for`, `try/catch`, los constructores, los metodos, y las rutas
   `sse`/`ws` que se apoyan en esta maquinaria.
3. **Tabla de builtins** — el puente de §4 hacia el motor nativo.
   *Hecho en el hito 2:* `text`, `html`, `json`, `render`, `status`, `redirect`,
   `send_file`, `len`, `str`, `int`, `header`, `query`.
4. ~~**Watcher + hot swap**~~ — hecho en el hito 1: sondeo de mtimes, swap de un
   `shared_ptr`, y si la nueva versión no compila se sigue sirviendo la anterior.
5. ~~**Diagnósticos**~~ — hecho en el hito 1: `fichero:línea:columna`, línea de código y
   cursor bajo la posición exacta.
6. **HMAC-SHA256 vendorizado** — ~200 líneas, cero dependencias.
7. **Store de estado compartido** — nativo, atómico, entre hilos.
8. ~~**`CMakeLists.txt`**~~ — hecho en el hito 0.

---

## 8. Estado de la documentación

- `README.md` — la tesis actual ("los intérpretes son lentos, por eso pierden Node y
  Python") se vuelve contra el proyecto y hay que reescribirla. La versión que sí se
  sostiene: el stack de I/O sigue siendo nativo y multicore, sin GIL ni pausas de GC
  globales, y el bytecode solo ejecuta pegamento, aislado por hilo.
- `GUIDE.md` — documenta la API de C++ que deja de ser pública. Se reescribe sobre Odio.
- **Discrepancia detectada en el hito 0:** README y GUIDE dicen que las plantillas son inja
  y que está vendorizado en `third_party/`. Es falso desde la migración a Jinja2Cpp: no
  estaba vendorizado, no había `CMakeLists.txt`, y **el repo no compilaba**. Al reescribir
  ambos documentos hay que corregir también la lista de dependencias vendorizadas y la
  promesa de "sin red durante cmake".
- `plan.md` — **obsoleto.** Planificaba migrar `SCHEMA` a reflexión estática C++26; el
  problema desaparece al declararse los structs en Odio.

---

## 9. Decisiones tomadas

| Tema | Decisión |
|---|---|
| Auth | HMAC-SHA256 vendorizado, HS256. Sin OpenSSL. Se pierde RS256 |
| Tiempo real | SSE **y** WebSockets |
| Uploads | Multipart se queda |
| Transporte | HTTP/1.1 en claro. TLS y HTTP/2 al reverse proxy |
| Ejecución | Bytecode sobre VM propio, un VM por hilo de event loop |
| Compilación | Interna al binario. Sin toolchain externo, sin transpilación a C++ |
| Persistencia | **Fuera de 2.0.** Llegará como módulos, no como builtin |
| Plantillas | **Jinja2Cpp**, no inja. Jinja2 de verdad, no un subconjunto |
| Sintaxis | Bloques por indentación (Python), tipado estático (C++), rutas (Flask), entrada por firma (FastAPI) |
| Genéricos | Solo contenedores nativos, borrados en compilación. Sin clases genéricas de usuario |
| Config | En Odio, bloque `app:`. Sin YAML ni TOML |
| Layout | `osodio ./mi-app` lee el árbol recursivamente. Orden indiferente, compilación en dos pasadas |

---

### Por qué Jinja2Cpp y no inja

inja es una sola cabecera sin dependencias, y sobre el papel encaja mejor con la filosofía de
vendorizado. Se descartó igualmente: **implementa un subconjunto de Jinja2 y diverge en
detalles**. Jinja2Cpp implementa Jinja2 de verdad — la sintaxis de Flask, que es la que
escribe la gente y la que generan por defecto los LLM cuando les pides una plantilla.

El coste es real y hay que asumirlo: Jinja2Cpp no está empaquetado en apt, no es cabecera
única y arrastra **Boost, fmt, rapidjson, expected-lite, optional-lite, variant-lite y
string-view-lite**. Se trae con `FetchContent` y un tag fijado (1.3.2), lo que **rompe la
promesa de "cero red durante cmake"** en el primer configure. Es la única dependencia en esa
situación.

Medido tras el hito 0, el coste está **enteramente en tiempo de compilación**:

| | |
|---|---|
| `_deps` en disco | 836 MB (648 MB solo Boost) |
| `libjinja2cpp.a` | 23,9 MB |
| `libosodio.a` (el motor) | **0,8 MB** |
| Binario final | 6,6 MB · **3,3 MB con `strip`** |
| Bibliotecas dinámicas | libstdc++, libm, libgcc, libc — **ninguna de Boost** |

Boost se usa como cabeceras: no aparece en `ldd` del binario. O sea que la objeción del
README a Boost.Asio — 200 MB de cabeceras que engordan la compilación — **sigue siendo
cierta aquí**, pero no toca lo que se despliega. Lo que se envía son 3,3 MB que solo
dependen del runtime de C++.

---

## 10. Futuro: módulos

La persistencia y todo lo que dependa de una tecnología externa concreta entra como
**módulos de Odio**, no como parte del núcleo:

- `sqlite` — embebido, un fichero, sin servidor
- `postgres` — vía libpq
- `mysql`

**Motivo de la separación:** el núcleo defiende eficiencia y no puede quedar atado a un
motor síncrono. Un módulo se carga solo si la app lo declara, paga su coste solo quien lo
usa, y cada uno puede resolver su propio modelo de concurrencia (pool de hilos para los
síncronos, I/O nativa para los que hablan por socket).

**Consecuencia para el diseño de la sintaxis, aunque no se implemente ahora:** el lenguaje
necesita un concepto de **espacio de nombres** desde el día uno (`sqlite.query(...)` frente
a nombres planos). Retrofitear namespacing a un lenguaje que nació plano es doloroso; dejar
la puerta abierta en la gramática es gratis.

---

## 11. Decisiones pendientes

- **Historia de tests.** ¿Odio tiene forma de testear endpoints en proceso (heredando
  `TestClient`), o los tests son externos vía HTTP? Si es lo primero, la gramática crece.
- **`else if` frente a `elif`.** La gramática recoge `else if`. Con bloques por indentación
  lo natural sería `elif`; con tipos de aire C, `else if`. Cambio de una línea.
