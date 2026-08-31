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
- `render(plantilla, datos)` — motor de plantillas propio (ver §9)
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

### Aparte: persistencia
El núcleo **no** habla con ninguna base de datos. La persistencia entra como **módulos**
(§10) que se importan y se configuran: atar el núcleo a un motor síncrono contradice el
argumento de eficiencia del proyecto.

Lo que el VM suspende, por tanto, es `ws.recv()` (bloquea hasta que llega un mensaje),
`sleep()` (bucles de keepalive de SSE) y las llamadas a un módulo de datos. La pila propia
del VM es obligatoria por los dos primeros, aunque no se importe ningún módulo.

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
  templates/         plantillas de Odio
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

## 6. Qué se cortó

Ya está hecho. Lo que sigue es el registro de lo que se fue y lo que quedó, con
las cuentas reales.

### Borrado entero

| Fichero | Líneas | Motivo |
|---|---:|---|
| `schema.hpp` — macro `SCHEMA` | 128 | Las clases se declaran en el `.odio` |
| `validation.hpp` | 64 | `validate:` es parte del lenguaje |
| `di.hpp` / `Inject<T>` | 81 | No queda código C++ de usuario donde inyectar |
| `group.hpp` | 112 | `group ... :` es parte del lenguaje |
| `testing.hpp` — `TestClient` | 225 | Las pruebas ejercen el binario por el socket |
| `third_party/simdjson.*` | — | El único usuario era `handler_traits` |

También se fueron TLS/OpenSSL, HTTP/2/nghttp2, `csrf.hpp` y `jwt.hpp` (estaban
enteros dentro de `#ifdef OSODIO_HAS_TLS`). JWT se rehízo sobre el HMAC-SHA256
propio de `src/odio/crypto.cpp`.

### Recortado

| Fichero | Antes | Ahora | Qué queda |
|---|---:|---:|---|
| `handler_traits.hpp` | 448 | 70 | Repartir `Request&`/`Response&` y aceptar `void` o `Task<void>` |
| `middleware.hpp` | 482 | 57 | `logger()`. `cors`, `compress`, `helmet` y `rate_limit` los pone el proxy |
| `openapi.hpp` | 347 | 69 | El HTML de Swagger UI. El documento lo genera el frontend desde el AST |
| `app.hpp` | 388 | 322 | Sin `provide()`, `group()`, `enable_docs()` ni la contabilidad de rutas |

Son **1.856 líneas menos** de código del proyecto (más los 174.000 de simdjson
vendorizado). La metaprogramación de plantillas se va porque existía únicamente
para que el usuario escribiera C++: el binding de argumentos ahora lo hace el
frontend al leer el `.odio`, una vez, con los nombres y los tipos delante.

### Lo que NO se cortó, y por qué

- **`io_uring_loop.cpp` sigue ahí.** Está detrás de `-DOSODIO_IO_URING=ON`, no se
  compila por defecto y no cuesta nada tenerlo. Cortarlo era adelgazar por
  adelgazar.
- **`api_info()` sigue en `App`.** Es almacenamiento simple —título y versión— que
  el binario lee para el documento OpenAPI.

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
   C++. Sobre eso van las rutas **`sse`** y **`ws`**, con `sse` y `ws` como objetos
   reservados. Tambien `group` con prefijos anidados y guardas acumuladas, y los
   objetos `session` y `jwt`, mas `on error` con `error` y `request` como objetos
   reservados, `for`, el almacen compartido `state`, `log`, cookies, formularios,
   multipart con `File`/`List<File>`, metodos sobre valores, `try/catch` y `/docs`
   generado desde el AST. **La superficie de la capa 2 esta cubierta**, mas
   funciones de usuario, constructores y metodos de clase. La persistencia entro
   despues como modulos (§10): sqlite, postgres y mysql.
3. **Tabla de builtins** — el puente de §4 hacia el motor nativo.
   *Hecho en el hito 2:* `text`, `html`, `json`, `render`, `status`, `redirect`,
   `send_file`, `len`, `str`, `int`, `header`, `query`, `sleep` (asincrono) y los
   miembros de `sse` y `ws` (`ws.recv` tambien asincrono).
4. ~~**Watcher + hot swap**~~ — hecho en el hito 1: sondeo de mtimes, swap de un
   `shared_ptr`, y si la nueva versión no compila se sigue sirviendo la anterior.
5. ~~**Diagnósticos**~~ — hecho en el hito 1: `fichero:línea:columna`, línea de código y
   cursor bajo la posición exacta.
6. ~~**HMAC-SHA256 vendorizado**~~ — hecho: SHA-256, HMAC, base64url y comparación
   en tiempo constante, verificados contra los vectores del RFC y contra `openssl`.
7. ~~**Store de estado compartido**~~ — hecho: `state.incr/decr/get/set/remove`,
   con mutex, verificado con 300 incrementos concurrentes sobre 16 hilos.
8. ~~**`CMakeLists.txt`**~~ — hecho en el hito 0.

---

## 8. Estado de la documentación

- `README.md` — **reescrito.** La tesis vieja ("los intérpretes son lentos") se volvía
  contra el proyecto; la nueva distingue lo que se interpreta de lo que no, y separa la
  crítica que sí se sostiene —operar entre tipos distintos— de la que no.
- `GUIDE.md` — **reescrito** sobre Odio.
- La discrepancia detectada en el hito 0 (README y GUIDE afirmaban que las plantillas eran
  inja y que estaba vendorizado; era falso desde la migración a Jinja2Cpp) queda corregida
  en la reescritura, incluida la promesa de "sin red durante cmake".
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
| Persistencia | Módulos `sqlite`, `postgres` y `mysql`, sobre pool de hilos y `await` |
| Plantillas | Motor propio, con la forma de Jinja2 y expresiones de Odio dentro |
| Sintaxis | Bloques por indentación (Python), tipado estático (C++), rutas (Flask), entrada por firma (FastAPI) |
| Genéricos | Solo contenedores nativos, borrados en compilación. Sin clases genéricas de usuario |
| Config | En Odio, bloque `app:`. Sin YAML ni TOML |
| Layout | `osodio ./mi-app` lee el árbol recursivamente. Orden indiferente, compilación en dos pasadas |

---

### Plantillas: de Jinja2Cpp a un motor propio

La primera decisión fue **Jinja2Cpp**, y el motivo era bueno: inja, que es una sola cabecera
sin dependencias, implementa un *subconjunto* de Jinja2 y diverge en detalles, mientras que
Jinja2Cpp implementa Jinja2 de verdad — la sintaxis de Flask, que es la que escribe la gente
y la que generan por defecto los LLM.

El coste se asumió a sabiendas: no está empaquetado en apt, no es cabecera única, y arrastra
**Boost, fmt, rapidjson, expected-lite, optional-lite, variant-lite y string-view-lite**. Se
traía con `FetchContent` y un tag fijado, lo que **rompía la promesa de "cero red durante
cmake"** en el primer configure. Era la única dependencia en esa situación.

**Se revirtió.** El motor de plantillas es ahora propio: conserva la forma de Jinja2 —`{{ }}`,
`{% if %}`, `{% for %}`, `{% include %}`, `{% extends %}`, `{% block %}`, `|safe`— pero lo
que va dentro de las llaves son **expresiones de Odio**, parseadas por el mismo parser y
ejecutadas por el mismo VM que el resto del lenguaje.

Los dos motivos, en orden de peso:

1. **Una errata en una plantilla pasa a ser un error de compilación.** La plantilla se
   compila cuando el emisor ve el `render("x.html", k=v)`, contra esas claves y sus tipos.
   `{{ quien.mayusculas() }}` sale en `osodio --check`, con fichero y línea, en vez de en
   producción. Ningún motor externo puede dar eso, porque no conoce el lenguaje de la app.
2. **El precio de la dependencia.** Medido al quitarla:

| | Con Jinja2Cpp | Con motor propio |
|---|---|---|
| Binario | 7.951.752 B | **1.592.032 B** |
| `build/_deps` | ~800 MB | **no existe** |
| Compilar desde cero | minutos | **19 s** |
| Red en el primer `cmake` | obligatoria | **ninguna** |

El rendimiento apenas entró en la decisión: renderizar 500 filas mejoró un 11%, y esa mejora
vino de cambiar de motor, no de quitar la biblioteca.

Lo que se pierde: los filtros de Jinja2 (`|upper`, `|join`, ...), que aquí son métodos de
Odio, y `{{ super() }}`, que no está.

---

## 10. Módulos de persistencia

La persistencia entra como **módulos**, no como parte del núcleo:

Cada módulo expone `query`, `exec`, `begin`, `commit`, `rollback` y `last_id`.

- `sqlite` — embebido, un fichero, sin servidor
- `postgres` — vía libpq
- `mysql` — vía libmysqlclient

Cada uno se compila solo si su cliente está presente (`cmake` lo informa al configurar), y
`import` de uno que no está da un error que enumera los disponibles.

**Motivo de la separación:** el núcleo defiende eficiencia y no puede quedar atado a un
motor síncrono. Un módulo se carga solo si la app lo declara, paga su coste solo quien lo
usa, y cada uno puede resolver su propio modelo de concurrencia (pool de hilos para los
síncronos, I/O nativa para los que hablan por socket).

**Concurrencia.** Los tres clientes tienen API bloqueante. Ejecutarlos en el hilo del event
loop clavaría ese core entero, así que una consulta **suspende el handler** igual que
`await sleep` y el trabajo ocurre en un pool propio del módulo. Cada worker es dueño de una
conexión, así que no hay reparto ni sincronización entre hilos.

Verificado contra un PostgreSQL 18 real, con `pool 4` y consultas de 1 s:

| Concurrentes | Tiempo |
|---|---|
| 1 | 1016 ms |
| 2 | 1015 ms |
| 4 | 1039 ms |
| 8 | 2015 ms (dos tandas) |
| 16 | 4018 ms (cuatro tandas) |

Satura en el tamaño del pool, encola el resto y drena por oleadas, sin perder ninguna
petición. Y una ruta sin base de datos responde en 10 ms con el pool saturado.

También verificado que si el servidor cae, la consulta devuelve un error como valor, y al
volver el servidor las conexiones se reabren solas.

**Transacciones.** `begin` fija la conexión del worker que la abrió, y todo lo que venga
después del mismo módulo va por ella hasta `commit` o `rollback`. Un handler que termina sin
cerrarla la deshace automáticamente y lo avisa por el log: si no, esa conexión volvería al
pool dentro de una transacción y el siguiente que la cogiera heredaría ese estado.

`last_id()` se encamina a la conexión del último `exec`, porque el identificador generado no
existe en las demás.

**Inyección de SQL.** Los parámetros van siempre por *bind* —`sqlite3_bind`, `PQexecParams`,
sentencias preparadas de MySQL— y nunca concatenados. No hay forma de construir SQL por
concatenación desde Odio.

---

## 11. Decisiones pendientes

- (resuelto) **Historia de tests.** Dos niveles, y ninguno necesita sintaxis nueva:
  `--autotest` hace que el binario recorra sus propios endpoints tras cada recarga, y
  `tests/run_tests.sh` es la suite de regresión del proyecto, que ejerce el binario por el
  socket.
- (resuelto) `elif` y `else if` se admiten los dos.
- **MySQL no se ha probado contra un servidor real.** Compila, enlaza y comparte el
  camino de sqlite y postgres —que sí están probados—, pero no se ha levantado un `mysqld`
  contra el que ejercerlo. Hasta que eso pase, el módulo es código sin verificar.
- (resuelto) **Los métodos HTTP son palabras reservadas en todo el fichero.** `get`,
  `post`, `put`, `patch`, `delete` y `any` no valen como nombre de variable, campo ni
  parámetro, aunque solo signifiquen algo delante de `endpoint`. Se podrían haber hecho
  contextuales; se ha decidido dejarlas reservadas. Con mayúscula no chocan —`class Post`
  y `Post p` compilan— y tras un `.` cualquier palabra clave es un nombre, así que
  `state.get(...)` funciona.
