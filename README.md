# Osodio

Un binario. Lee ficheros `.odio` y sirve.

```odio
app:
    name      "Mi blog"
    port      8080
    templates "./plantillas"

    static "/static" -> "./publico"
    docs

    session:
        secret env("SESSION_SECRET")


class Articulo:
    int     id
    string  titulo
    string  cuerpo
    string? etiquetas

    validate:
        titulo != ""        "titulo: obligatorio"
        len(cuerpo) >= 20   "cuerpo: minimo 20 caracteres"


get endpoint("/"):
    return render("indice.html")

get endpoint("/articulos/:id", int id):
    return { "id": id, "titulo": "Hola mundo" }

post endpoint("/articulos", Articulo a):
    # Si el cuerpo no parsea o no valida, la respuesta es 422 y esto no
    # llega a ejecutarse nunca.
    return { "creado": a.titulo }.status(201)


group("/admin"):
    require session.rol == "admin" else redirect("/login")

    get endpoint("/panel"):
        return render("panel.html", quien=session.usuario)
```

```
$ osodio ./mi-blog
osodio: 3 fichero(s), 5 ruta(s) — 2 declarativa(s), 3 con logica
Osodio running on http://0.0.0.0:8080 (threads=16, press CTRL+C to quit)
```

Guardas el fichero y se recarga. Sin recompilar, sin reiniciar, sin CMake.

---

## Por qué existe

Osodio empezó siendo un framework de C++ con la ergonomía de FastAPI. Funcionaba, y
seguía teniendo tres problemas que ninguna cantidad de plantillas iba a arreglar:

**Cambiar un endpoint obligaba a recompilar.** El ciclo de trabajo de un framework web no
puede medirse en decenas de segundos.

**Intentaba hacer de todo.** TLS, HTTP/2, compresión, rate limiting, cabeceras de
seguridad. Un esqueleto debe servir ficheros de forma segura, no reimplementar lo que
nginx ya hace mejor.

**Seguía siendo verboso.** Macros para declarar esquemas, plantillas para extraer
parámetros, firmas de lambda de tres líneas.

Osodio 2.0 responde a los tres a la vez: la declaración de endpoints se hace en **Odio**,
un lenguaje propio, y el binario lo compila al arrancar y cuando detecta un cambio.

### "¿No era que los intérpretes eran lentos?"

La versión anterior de este README despotricaba contra Node y Python por su rendimiento.
Ese argumento sigue en pie, y por eso conviene ser preciso sobre qué se interpreta aquí y
qué no.

**El stack de I/O sigue siendo nativo y multinúcleo.** Un event loop por core, `SO_REUSEPORT`,
sin GIL, sin GC global. El parseo HTTP es llhttp; el JSON, nlohmann y simdjson; el
servicio de estáticos, `sendfile(2)`. Nada de eso cambia.

**El bytecode solo ejecuta pegamento.** Resolver un nombre, llamar a un builtin nativo,
encadenar el resultado. Y ni siquiera siempre: una ruta que se resuelve entera en
compilación —`return render("index.html")`— se convierte en una acción nativa y **no
ejecuta ni un paso de bytecode**. El propio binario te dice cuántas rutas van por cada
camino al arrancar.

**Cada VM está aislado por petición**, alojado en el marco de la corrutina del handler. Dos
handlers suspendidos sobre el mismo core no comparten ni pila ni heap, así que no hay nada
que sincronizar entre cores. El estado común existe, pero es explícito y atómico.

Lo que Odio **no** hereda de JavaScript es su otro pecado, el que de verdad duele:

```odio
1 + "1"     # error, no "11"
0 == "0"    # false, no true
```

Operar entre tipos distintos es un error, no una conversión a tus espaldas. Para unir un
número a una cadena hay que decirlo: `"n = " + str(n)`.

### Corre detrás de un reverse proxy

Osodio 2.0 no habla TLS ni HTTP/2. Los hace nginx o Caddy, mejor y desde hace años. El
proxy es dueño del transporte; Osodio es dueño de la aplicación. Eso es lo que permite que
el binario pese **3,5 MB** y no enlace OpenSSL ni nghttp2.

---

## El lenguaje

Bloques por indentación como Python, tipado estático como C++. La forma de declarar rutas
viene de Flask; la de recibir datos, de FastAPI.

### Todo entra por la firma

```odio
get endpoint("/usuarios/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "buscando": q }
```

`:id` se enlaza al segmento de ruta; `page` y `q` a la query, con valor por defecto si lo
tienen. **El compilador verifica que cada `:nombre` del patrón tiene quien lo recoja, y al
revés** — algo que Flask no puede hacer porque el tipo va dentro de una cadena.

Un parámetro cuyo tipo es una clase se enlaza al **cuerpo** de la petición, con validación
y 422 automáticos. Uno de tipo `File` o `List<File>`, a las partes multipart.

### Todo sale por `return`

| | |
|---|---|
| `return { "a": 1 }` | 200, JSON |
| `return render("x.html", k=v)` | HTML con Jinja2 |
| `return text("hola")` / `html(...)` | texto plano / HTML |
| `return send_file(ruta)` | fichero, `sendfile(2)` |
| `return redirect("/otro")` | 302 |
| `return status(204)` | código sin cuerpo |

No hay objeto `response` mutable que arrastrar por el handler.

### `require` en lugar de middleware

```odio
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/yo"):
        return { "sub": jwt.claims["sub"] }
```

Tras delegar CORS, compresión y rate limiting al proxy, el único middleware que quedaba era
proteger rutas. Y eso es azúcar de `if not X: return Y`, no un concepto aparte. Los grupos
anidan y las guardas se acumulan.

### Objetos reservados

Existen sin declararse, dentro del contexto que los define.

| Objeto | Dónde | Qué da |
|---|---|---|
| `request` | cualquier handler | `path`, `method`, `ip` |
| `session` | cualquier handler | cookie firmada, cualquier campo |
| `jwt` | cualquier handler | `valid`, `claims` |
| `state` | cualquier handler | almacén compartido entre hilos |
| `log` | en todas partes | `info`, `warn`, `error` |
| `sse` | rutas `sse` | `send`, `ping`, `open` |
| `ws` | rutas `ws` | `send`, `recv`, `open`, `close` |
| `error` | bloques `on error` | `code`, `message` |

Usar `sse` en una ruta `get`, o `error` fuera de un `on error`, es error **de compilación**.

---

## Ejemplos

### Login con sesión y rol

```odio
class Login:
    string nombre
    string contrasena

    validate:
        nombre != ""      "nombre: obligatorio"
        contrasena != ""  "contrasena: obligatoria"

post endpoint("/login", Login datos):
    if datos.contrasena != "hunter2":
        return status(401)

    session.usuario = datos.nombre
    session.rol     = datos.nombre == "alice" ? "admin" : "user"
    return redirect("/")

post endpoint("/logout"):
    session.clear()
    return redirect("/")

group("/admin"):
    require session.rol == "admin" else status(403)

    get endpoint("/panel"):
        return render("panel.html", de=session.usuario)
```

`session` es una cookie firmada con HMAC-SHA256, sin estado en servidor. Va `HttpOnly`
siempre y solo se reescribe si el handler la toca. Una firma inválida deja la sesión
vacía, nunca a medias.

### Tiempo real

```odio
sse endpoint("/metricas"):
    int tick = 0
    while sse.open:
        await sleep(2000)
        tick = tick + 1
        sse.send("delta", "{\"tick\":" + str(tick) + "}", str(tick))

ws endpoint("/chat") origins("https://miapp.com"):
    ws.send("bienvenido")
    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break
        ws.send("eco: " + msg)
```

`await` suspende el handler sin bloquear el event loop: ocho peticiones de 500 ms
concurrentes tardan 500 ms, no cuatro segundos.

`origins(...)` es **obligatorio** en una ruta `ws`, y su ausencia es error de compilación.
Sin lista blanca, cualquier web puede abrir la conexión desde el navegador de tu usuario.

### Subida de ficheros

```odio
post endpoint("/avatar", File imagen):
    require imagen.content_type.starts_with("image/") else status(415)
    require imagen.size <= 5 * 1024 * 1024            else status(413)
    return { "guardado": imagen.save("./subidas") }

post endpoint("/galeria", List<File> fotos):
    List<string> nombres = []
    for File f in fotos:
        nombres.add(f.save("./subidas"))
    return { "nombres": nombres }
```

`save()` se queda solo con el nombre de fichero: un `filename` con `..` o absoluto no puede
escapar del directorio.

### Estado compartido

```odio
get endpoint("/visitas"):
    return { "n": state.incr("visitas") }
```

`state` es la única vía de estado común entre los N event loops. Expone operaciones y no
propiedades a propósito: `state.x = state.x + 1` sería una carrera entre la lectura y la
escritura.

### Páginas de error propias

```odio
on error 404:
    return render("404.html", ruta=request.path)

on error:
    log.error(error.message)
    return render("500.html")
```

---

## Errores

Un compilador con malos mensajes da peor experiencia que recompilar. Todo sale con fichero,
línea, columna y cursor:

```
./app.odio:12:19: error: el patron declara ':id' pero ningun parametro lo recoge
  12 | get endpoint("/usuarios/:id"):
     |              ^
```

Y se comprueba al compilar mucho más de lo que parece: un campo inexistente en una regla de
`validate`, un `await` que falta o que sobra, un `sse` fuera de su ruta, una clase
duplicada, dos cuerpos en la misma ruta, un `ws` sin `origins`.

**Si el fichero que guardas no compila, se sigue sirviendo la versión anterior.** Un typo
no tumba el servidor.

---

## Qué trae

| | |
|---|---|
| **Rutas** | Radix tree, `:param`, `{param}`, `*`, grupos anidados |
| **Entrada** | Ruta, query con defecto, cuerpo JSON tipado, multipart, cabeceras, cookies, formularios |
| **Validación** | Bloque `validate:` por clase → 422 automático con todos los mensajes |
| **Salida** | JSON, HTML, texto, plantillas Jinja2, ficheros, redirecciones, códigos |
| **Async** | `await sleep(ms)`, `await ws.recv()`, cancelación al desconectar |
| **Tiempo real** | SSE con `id:` para reconexión, WebSockets RFC 6455 |
| **Auth** | `session` en cookie firmada, JWT HS256 con verificación de `alg`, `exp` e `iss` |
| **Estado** | Almacén compartido con operaciones atómicas |
| **Ficheros** | MIME, ETag, 304, `sendfile(2)`, SPA, bloqueo de dotfiles y traversal |
| **Lenguaje** | Clases, `for`, `while`, `try/catch`, ternario, listas, diccionarios, métodos |
| **Docs** | `/openapi.json` y `/docs` generados desde el AST |
| **Observabilidad** | Logger con rotación, `/health`, `/metrics` Prometheus |
| **Recarga** | Vigilancia de ficheros e intercambio atómico del módulo |

### Qué no trae, a propósito

**TLS y HTTP/2** — del reverse proxy.
**CORS, compresión, rate limiting, cabeceras de seguridad** — del reverse proxy.
**Persistencia** — llegará como módulos (`sqlite`, `postgres`, `mysql`). Atar el núcleo a
un motor síncrono contradice el argumento de eficiencia.
**Clases genéricas de usuario** — `List<T>` y `Dict<K,V>` sí; `class Caja<T>` no.

---

## Compilar

Requiere **Linux** (epoll, `sendfile(2)`, `SO_REUSEPORT`), **CMake 3.20+**, **C++20**
(GCC 11+ o Clang 13+) y **zlib**.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Dependencias vendorizadas en `third_party/`: nlohmann/json, simdjson y llhttp.

**Jinja2Cpp es la excepción**: no está empaquetado, no es cabecera única y arrastra Boost,
fmt y rapidjson. Se trae con `FetchContent` y un tag fijado, así que el **primer** configure
necesita red. Se eligió sobre inja porque implementa Jinja2 de verdad —la sintaxis de
Flask— en vez de un subconjunto. El coste es de disco y de tiempo de compilación: el
binario final son 3,5 MB y no enlaza nada de Boost.

```bash
osodio ./mi-app          # todos los .odio del directorio, recursivamente
osodio app.odio          # solo ese fichero
osodio a.odio b.odio     # solo esos
osodio ./app --check     # compila y sale
osodio ./app --no-watch  # sin recarga en caliente
```

---

## Estado

Osodio 2.0 está en desarrollo. El motor, el lenguaje y toda la superficie descrita aquí
funcionan y están cubiertos por los ejemplos del repositorio (`ejemplo-*.odio`).

El diseño completo, con las decisiones tomadas y sus motivos, está en
[OSODIO-2.0.md](OSODIO-2.0.md). La gramática formal del lenguaje, en
[ODIO-GRAMMAR.md](ODIO-GRAMMAR.md).
