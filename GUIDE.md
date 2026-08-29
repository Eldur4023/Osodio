# Osodio — Guía del desarrollador

> Referencia práctica de Odio, el lenguaje que interpreta Osodio 2.0. La gramática formal
> está en [ODIO-GRAMMAR.md](ODIO-GRAMMAR.md); las decisiones de diseño y sus motivos, en
> [OSODIO-2.0.md](OSODIO-2.0.md).

## Índice

1. [Poner en marcha](#1-poner-en-marcha)
2. [Estructura de un proyecto](#2-estructura-de-un-proyecto)
3. [El bloque `app:`](#3-el-bloque-app)
4. [Rutas](#4-rutas)
5. [Parámetros](#5-parámetros)
6. [Clases y validación](#6-clases-y-validación)
7. [Respuestas](#7-respuestas)
8. [Grupos y guardas](#8-grupos-y-guardas)
9. [Sesión](#9-sesión)
10. [JWT](#10-jwt)
11. [Asincronía](#11-asincronía)
12. [Base de datos](#12-base-de-datos)
13. [Server-Sent Events](#13-server-sent-events)
14. [WebSockets](#14-websockets)
15. [Estado compartido](#15-estado-compartido)
16. [Subida de ficheros](#16-subida-de-ficheros)
17. [Manejadores de error](#17-manejadores-de-error)
18. [Funciones](#18-funciones)
19. [El lenguaje](#19-el-lenguaje)
20. [Referencia de builtins](#20-referencia-de-builtins)
21. [Errores frecuentes](#21-errores-frecuentes)
22. [Cómo funciona por dentro](#22-cómo-funciona-por-dentro)

---

## 1. Poner en marcha

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requiere Linux (epoll, `sendfile(2)`, `SO_REUSEPORT`), CMake 3.20+ y C++20. El primer
`configure` necesita red para traer Jinja2Cpp, que es la única dependencia que no está
vendorizada.

Opcional pero recomendado: `sudo apt install libjemalloc-dev`. Con varios event loops y un
pool de base de datos, el `malloc` de glibc serializa en sus arenas; cambiarlo vale más que
cualquier optimización del código en respuestas JSON grandes. Si está, `cmake` lo enlaza
solo; si no, compila igual y lo dice.

El argumento decide qué se compila, sin sorpresas:

| Invocación | Qué compila |
|---|---|
| `osodio app.odio` | Solo ese fichero |
| `osodio a.odio b.odio` | Solo esos dos |
| `osodio ./mi-app` | Todos los `.odio` del directorio, recursivamente |

| Opción | |
|---|---|
| `--check` | Compila y sale, sin arrancar |
| `--port N` | Sobrescribe el puerto del bloque `app:` |
| `--no-watch` | No vigila cambios |
| `--verbose` | Registra por consola cada petición que llega. Cuesta ~25% del rendimiento, así que no viene puesto |
| `--autotest` | Al arrancar y en cada recarga, recorre los endpoints |
| `--autotest=all` | Incluye también POST/PUT/PATCH/DELETE |

### Auto-prueba

Con `--autotest`, tras arrancar y tras **cada recarga con éxito**, Osodio se pega a sí mismo
por HTTP y recorre las rutas del módulo:

```
cambios detectados: recompilando
recargado: 11 ruta(s) — 4 declarativa(s), 7 con logica
autotest: probando 11 ruta(s)
  ok        GET    /usuarios/1                       200  0ms
  ERROR     GET    /rota                             500  0ms
  rechazada GET    /admin/panel                      403  0ms
  flujo     GET    /eventos                          200  0ms
  omitida   WS     /chat   (necesita handshake de WebSocket)
autotest: 8 ok, 2 rechazadas, 1 con error, 1 omitidas
```

No comprueba lógica de negocio: busca **que ningún handler se rompa** después de un cambio.
Por eso un `5xx` es lo único que cuenta como error — un `4xx` puede ser el comportamiento
correcto de una guarda o de una validación.

Los parámetros de ruta se rellenan por tipo (`:id` de tipo `int` → `1`), y con
`--autotest=all` el cuerpo se sintetiza a partir de la clase que la ruta espera.

**Sobre los efectos secundarios:** probar un endpoint *ejecuta su handler*. Un `DELETE`
haría su trabajo de verdad en cada recarga, así que por defecto solo se recorren `GET`,
`HEAD` y `SSE`. Incluir el resto es una decisión de quien lanza el binario, no del binario.

El watcher vigila exactamente el conjunto que se compiló. Al guardar, recompila y sustituye
el módulo. **Si el fichero nuevo no compila, se sigue sirviendo el anterior** y el error se
imprime con fichero, línea y columna.

---

### Pruebas del propio Osodio

```bash
cd build && ctest --output-on-failure
# o directamente:
tests/run_tests.sh ~/osodio-build/osodio
```

60 pruebas que levantan el binario contra ficheros `.odio` reales y comprueban las
respuestas por el socket. La suite no enlaza nada del proyecto: prueba lo que se despliega,
no una versión instrumentada de ello.

---

## 2. Estructura de un proyecto

```
mi-app/
  app.odio           configuración
  rutas/
    publico.odio
    admin.odio
  templates/         plantillas Jinja2
  public/            estáticos
```

El orden entre ficheros es indiferente: la compilación es en dos pasadas, primero se
recogen las declaraciones y después se resuelven los nombres. Una clase puede usarse antes
de declararse, y estar en otro fichero.

---

## 3. El bloque `app:`

Puede estar en cualquier fichero, pero **solo una vez**.

```odio
app:
    name      "Mi aplicación"
    version   "1.0.0"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    static "/"       -> "./dist" spa

    docs                      # /openapi.json y /docs
    health                    # /health
    metrics                   # /metrics, formato Prometheus

    session:
        secret  env("SESSION_SECRET")
        max_age 86400
        secure  true

    jwt:
        secret env("JWT_SECRET")
        issuer "mi-app"
```

`env("VAR")` se resuelve **al compilar**. Es la forma de que un secreto no acabe escrito en
el `.odio`.

`spa` en un montaje estático hace que las rutas no encontradas caigan en `index.html`.

---

## 4. Rutas

```odio
get    endpoint("/ruta"):
post   endpoint("/ruta"):
put    endpoint("/ruta"):
patch  endpoint("/ruta"):
delete endpoint("/ruta"):
any    endpoint("/ruta"):
sse    endpoint("/ruta"):                       # flujo de eventos
ws     endpoint("/ruta") origins("https://x"):  # WebSocket
```

Patrones: `/usuarios/:id`, `/usuarios/{id}`, `/ficheros/*`.

### Los dos niveles de ruta

Una ruta cuyo cuerpo se resuelve entero en compilación —un único `return` de un valor
constante o de una llamada nativa con argumentos literales— se convierte en una **acción
nativa** y no ejecuta ni un paso de bytecode:

```odio
get endpoint("/"):
    return render("index.html")        # declarativa: cero bytecode
```

El resto ejecuta bytecode. Al arrancar, el binario dice cuántas van por cada camino:

```
osodio: 3 fichero(s), 12 ruta(s) — 5 declarativa(s), 7 con logica
```

Una ruta con guardas de grupo **nunca** es declarativa: la acción nativa no las ejecutaría.

---

## 5. Parámetros

Todo lo que el handler necesita se declara en la firma.

```odio
get endpoint("/usuarios/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "q": q }
```

| Forma | De dónde sale |
|---|---|
| Nombre que aparece en el patrón | Segmento de la ruta |
| Nombre que no aparece | Query string |
| `= valor` | Valor por defecto si falta en la query |
| Tipo que es una `class` | Cuerpo JSON, con validación |
| `File` / `List<File>` | Partes multipart |

Tipos escalares: `int`, `long`, `float`, `double`, `bool`, `string`.

**El compilador verifica las dos direcciones**: que cada `:nombre` del patrón tiene un
parámetro que lo recoge, y que ningún parámetro de ruta sobra.

Un valor que no encaja con su tipo es un **400**, no una excepción:

```json
{"error":"parametro invalido","esperado":"int","param":"id","recibido":"abc"}
```

---

## 6. Clases y validación

```odio
class Usuario:
    int     id
    string  nombre
    int     edad
    string? contrasena          # ? = puede faltar

    validate:
        nombre != ""    "nombre: obligatorio"
        edad >= 0       "edad: no puede ser negativa"
        edad < 150      "edad: valor poco creible"
```

Usada como parámetro, se enlaza al cuerpo:

```odio
post endpoint("/usuarios", Usuario u):
    # Aquí `u` siempre es válido.
    return { "creado": u.nombre }
```

| Situación | Respuesta |
|---|---|
| Cuerpo que no es JSON | `400 {"error":"JSON invalido"}` |
| Falta un campo obligatorio | `422` con `"campo: obligatorio"` |
| Tipo equivocado | `422` con `"campo: se esperaba int"` |
| Una regla de `validate` falla | `422` con su mensaje |

**Los mensajes salen todos a la vez**, no el primero. Y el handler no llega a ejecutarse.

No hay coerción: `"30"` en un campo `int` es un 422, no se parsea.

Las reglas de `validate` **se compilan**, así que un campo mal escrito en una regla es un
error de compilación y no llega a producción:

```
./app.odio:9:9: error: 'nombrre' no esta declarada
```

### Constructores

```odio
class Punto:
    int x
    int y

    Punto(int x):              # con cuerpo
        this.x = x
        this.y = 0

    Punto(int x, int y)        # sin cuerpo: cada parámetro va a su campo
```

Se distinguen por el **número** de parámetros. Sin ninguno declarado, se ofrece uno con
todos los campos en orden de declaración. Los campos que el constructor no toque valen
`null`.

### Métodos

```odio
class Punto:
    int x
    int y

    fn int cuadrado():
        return this.x * this.x + this.y * this.y

    fn string etiqueta(string prefijo = "P"):
        return prefijo + "(" + str(this.x) + "," + str(this.y) + ")"

    fn Punto desplazado(int dx, int dy):
        return Punto(this.x + dx, this.y + dy)
```

```odio
Punto p = Punto(3, 4)
p.cuadrado()            # 25
p.etiqueta("Q")         # "Q(3,4)"
```

Métodos y constructores se compilan como funciones con `this` de primer parámetro, así que
usan la misma pila de marcos y admiten recursión y valores por defecto igual que `fn`.

La llamada **se resuelve al compilar** a partir del tipo declarado del receptor, así que un
método mal escrito no llega a producción:

```
./app.odio:8:20: error: 'P' no tiene un metodo 'triple'
```

Una instancia construida y una enlazada del cuerpo de la petición son lo mismo: los métodos
funcionan igual sobre las dos.

---

## 7. Respuestas

Todo sale por `return`. No hay objeto `response` que arrastrar.

```odio
return { "clave": "valor" }              # 200, JSON
return [1, 2, 3]                         # 200, JSON
return render("pagina.html", k=v)        # HTML con Jinja2
return text("hola")                      # text/plain
return html("<h1>hola</h1>")             # text/html
return send_file("/var/f.pdf")           # sendfile(2)
return redirect("/otro")                 # 302
return redirect("/otro", 301)            # 301
return status(204)                       # código sin cuerpo
```

Un handler que no devuelve nada y no escribe respuesta produce **204**.

### Encadenado

```odio
return { "id": 1 }.status(201)
return { "a": 1 }.header("X-Cosa", "valor")
return render("x.html").status(203)

return { "ok": true }.cookie("tema", "oscuro",
                             max_age=3600, http_only=false, same_site="strict")
```

Opciones de `cookie`: `max_age`, `path`, `domain`, `secure`, `http_only`, `same_site`
(`"lax"`, `"strict"`, `"none"`). Por defecto: `path=/`, `HttpOnly`, `SameSite=Lax`.

---

## 8. Grupos y guardas

```odio
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/yo"):
        return { "sub": jwt.claims["sub"] }

    group("/admin"):
        require jwt.claims["rol"] == "admin" else status(403)

        get endpoint("/stats"):
            return { "usuarios": state.get("usuarios", 0) }
```

Los prefijos se concatenan y **las guardas se acumulan**: para llegar a `/api/v1/admin/stats`
hay que pasar la del grupo padre y luego la propia.

`require X else Y` es azúcar de `if not X: return Y`. Funciona en cualquier posición, no
solo en un grupo. No hay concepto de middleware.

Las guardas van **antes** que las rutas dentro del bloque.

---

## 9. Sesión

Cookie firmada con HMAC-SHA256, al estilo Flask. Sin estado en servidor.

```odio
post endpoint("/login", Login datos):
    session.usuario = datos.nombre
    session.rol     = "admin"
    return redirect("/")

get endpoint("/quien"):
    return { "usuario": session.usuario, "rol": session.rol }

post endpoint("/logout"):
    session.clear()
    return redirect("/")
```

`session.<lo-que-sea>` admite cualquier nombre: es un almacén, no un objeto de campos
fijos. Un campo que no existe vale `null`.

- Necesita `session: secret ...` en el bloque `app:`. Sin él, tocarla es un error de ejecución.
- Va **`HttpOnly`** siempre; `Secure` según la configuración.
- Solo se reescribe la cookie si el handler la modifica.
- El contenido va **firmado pero no cifrado**: el usuario puede leerlo, solo no puede
  falsificarlo. No guardes ahí nada que no pueda ver.
- Una firma inválida deja la sesión vacía, nunca a medias.

---

## 10. JWT

HS256, verificado sobre la cabecera `Authorization: Bearer ...`.

```odio
group("/api"):
    require jwt.valid else status(401)

    get endpoint("/yo"):
        return { "sub": jwt.claims["sub"], "rol": jwt.claims["rol"] }
```

Se comprueban firma, `exp` e `iss` (si se configuró `issuer`). **Se rechaza cualquier `alg`
que no sea HS256, incluido `none`**: aceptar el algoritmo que declara el propio token es la
vulnerabilidad clásica de las librerías de JWT.

RS256 no está: requeriría criptografía asimétrica, y Osodio 2.0 no enlaza OpenSSL.

---

## 11. Asincronía

```odio
get endpoint("/lento/:ms", int ms):
    await sleep(ms)
    return { "esperado": ms }
```

`await` suspende el handler y devuelve el control al event loop. Ocho peticiones de 500 ms
concurrentes tardan 500 ms, no cuatro segundos.

`sleep()` despierta antes si el cliente se desconecta, y en ese caso el handler no continúa.

Reglas comprobadas al compilar:

- Un builtin asíncrono **obliga** a `await`: `sleep(100)` a secas es un error.
- Uno síncrono **lo prohíbe**: `await text("x")` es un error.
- `await` solo se aplica a una llamada asíncrona: `await 5` es un error.

Asíncronos disponibles: `sleep(ms)` y `ws.recv()`.

---

## 12. Base de datos

Tres módulos: `sqlite`, `postgres` y `mysql`. Se importan y se configuran; la conexión la
gestionan ellos.

```odio
import postgres

app:
    postgres:
        host     "127.0.0.1"
        port     5432
        database "mi_app"
        user     "odio"
        password env("PG_PASSWORD")
        pool     4
```

Cada módulo se compila solo si su cliente estaba presente al compilar Osodio. Si no,
`import postgres` da un error al compilar el `.odio`, no un fallo raro en producción.

### Configuración

| Módulo | Claves |
|---|---|
| `sqlite` | `file` (obligatoria), `pool`, `timeout_ms` |
| `postgres` | `url`, o bien `host` / `port` / `database` (obligatoria) / `user` / `password`; `pool` |
| `mysql` | `host` / `port` / `database` / `user` / `password`; `pool` |

`pool` es el número de conexiones, entre 1 y 64. Por defecto 4. Usa `env()` para las
contraseñas: se resuelve al compilar y no queda escrita en el `.odio`.

### Consultar

```odio
get endpoint("/articulos"):
    return await postgres.query("select id, titulo from articulos order by id")

get endpoint("/articulos/:id", int id):
    List<Json> filas = await postgres.query("select titulo from articulos where id = $1", id)
    if len(filas) == 0:
        return status(404)
    return filas[0]
```

`query()` devuelve `List<Json>`: una lista de diccionarios, con los tipos del motor
convertidos a los de Odio —entero, decimal, booleano, cadena y `null`—.

```odio
post endpoint("/articulos", Articulo a):
    int filas = await postgres.exec(
        "insert into articulos (titulo, vistas) values ($1, $2)", a.titulo, a.vistas)
    int id = await postgres.last_id()
    return { "id": id }.status(201)
```

`exec()` devuelve el número de filas afectadas. `last_id()` devuelve el último id
autogenerado.

**Los parámetros van siempre aparte, nunca concatenados.** El marcador depende del motor:
`?` en sqlite y mysql, `$1`, `$2`… en postgres. Concatenar la consulta a mano es la única
forma de abrirse a una inyección, y el lenguaje no te la pone fácil.

### Transacciones

```odio
post endpoint("/transfiere"):
    await sqlite.begin()
    await sqlite.exec("update cuentas set saldo = saldo - 30 where id = 1")
    await sqlite.exec("update cuentas set saldo = saldo + 30 where id = 2")
    await sqlite.commit()
    return { "ok": true }
```

`begin()` fija la conexión: todo lo que venga después en esa petición va por la misma, y
`commit()` o `rollback()` la sueltan. Si el handler termina —o revienta— con una
transacción abierta, Osodio hace `ROLLBACK` y lo avisa por consola. Sin eso, la siguiente
petición que cogiera esa conexión del pool heredaría el estado.

### Errores

Un error del motor no revienta el handler: llega como un diccionario con `error`.

```odio
get endpoint("/malo"):
    Json r = await sqlite.query("select * from no_existe")
    return r          # { "error": "no such table: no_existe" }
```

### Por qué no bloquea

Los clientes de sqlite, libpq y libmysqlclient son síncronos. Cada módulo mantiene un pool
de hilos con **una conexión por trabajador**; el `await` encola el trabajo, suelta el event
loop y lo retoma cuando el hilo termina. Ningún `query()` para el event loop, que es lo que
haría que todo el argumento de eficiencia se cayera.

---

## 13. Server-Sent Events

```odio
sse endpoint("/metricas/:cada", int cada):
    int tick = 0
    sse.send("snapshot", "{\"arranque\":true}")

    while sse.open:
        await sleep(cada)
        tick = tick + 1
        sse.send("delta", "{\"tick\":" + str(tick) + "}", str(tick))
        if tick % 10 == 0:
            sse.ping("keepalive")
```

| Llamada | Trama |
|---|---|
| `sse.send(datos)` | `data: ...` |
| `sse.send(evento, datos)` | `event: ...` + `data: ...` |
| `sse.send(evento, datos, id)` | añade `id:`, para reconexión con `Last-Event-ID` |
| `sse.ping(texto)` | comentario `: ...`, ignorado por el navegador |
| `sse.open` | falso cuando la conexión se cierra |

El flujo se abre antes de ejecutar el handler y se cierra al terminar. No hay respuesta
final que devolver.

---

## 14. WebSockets

```odio
ws endpoint("/eco") origins("https://miapp.com", "http://localhost:5173"):
    int n = 0
    ws.send("bienvenido")

    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break

        n = n + 1
        if msg == "adios":
            ws.send("cerrando tras " + str(n) + " mensajes")
            ws.close()
            break

        ws.send("eco " + str(n) + ": " + msg)
```

`await ws.recv()` devuelve el texto del mensaje, o `null` cuando la conexión se cierra.

**`origins(...)` es obligatorio**, y su ausencia es error de compilación. Los navegadores no
aplican la política de mismo origen al handshake de WebSocket: sin lista blanca, cualquier
web puede abrir la conexión desde el navegador de tu usuario y heredar sus cookies. Un
origen fuera de la lista recibe `403`.

---

## 15. Estado compartido

```odio
get endpoint("/visitas"):
    return { "n": state.incr("visitas") }

get endpoint("/contador"):
    return { "n": state.get("visitas", 0) }
```

| | |
|---|---|
| `state.incr(clave)` / `state.incr(clave, n)` | Suma y devuelve el valor nuevo |
| `state.decr(clave)` / `state.decr(clave, n)` | Resta |
| `state.get(clave)` / `state.get(clave, defecto)` | Lee |
| `state.set(clave, valor)` | Escribe |
| `state.remove(clave)` | Borra |

Es la **única** vía de estado común entre los event loops: cada VM tiene su pila y su heap y
no comparte nada. Por eso expone operaciones y no propiedades — `state.x = state.x + 1`
sería una carrera entre la lectura y la escritura.

Vive en memoria del proceso: se pierde al reiniciar, y no se comparte entre máquinas.

---

## 16. Subida de ficheros

```odio
post endpoint("/avatar", File imagen):
    require imagen.content_type.starts_with("image/") else status(415)
    require imagen.size <= 5 * 1024 * 1024            else status(413)
    string nombre = imagen.save("./subidas")
    return { "url": "/static/subidas/" + nombre }

post endpoint("/galeria", List<File> fotos):
    List<string> nombres = []
    for File f in fotos:
        nombres.add(f.save("./subidas"))
    return { "nombres": nombres }
```

Un `File` tiene `name`, `filename`, `content_type` y `size`, y el método `save(directorio)`,
que devuelve el nombre con el que se guardó.

`save()` se queda solo con el componente de fichero del nombre: un `filename` con `..` o
absoluto no puede escapar del directorio de destino.

Con `File` (no `List<File>`), la ausencia del fichero es un `422`. Con `List<File>`, una
lista vacía.

---

## 17. Manejadores de error

```odio
on error 404:
    return render("404.html", ruta=request.path, metodo=request.method)

on error 403:
    return { "error": "no puedes entrar aqui", "codigo": error.code }

on error:
    log.error(error.message)
    return render("500.html")
```

En un `on error 422`, `error.messages` trae la lista completa de mensajes de validación —
vacía si el 422 no vino de validar un cuerpo:

```odio
on error 422:
    return { "detalles": error.messages }
```

Sin código, es el manejador global. **Solo cubre 400–599**: con un 2xx el handler de la ruta
ya ha escrito la respuesta, y sustituirla sería un filtro de respuesta — es decir,
middleware, que Osodio 2.0 delega al proxy a propósito.

El código de estado se conserva. Si el manejador no escribe nada, se mantiene el cuerpo por
defecto.

---

## 18. Funciones

```odio
fn int doble(int x):
    return x * 2

fn string saluda(string nombre, string tratamiento = "hola"):
    return tratamiento + ", " + nombre

fn bool es_correo(string s):
    return s.contains("@") and s.contains(".")
```

El orden de declaración no importa: una función puede llamar a otra declarada más abajo, o
en otro fichero. La recursión funciona, con un tope de 200 llamadas anidadas — pasarse da
un error del lenguaje, no agota la memoria del proceso.

Los parámetros admiten valor por defecto, y los que faltan se rellenan en la llamada. Un
parámetro sin defecto no puede ir después de uno que lo tiene.

Una función sin `return` devuelve `null`. Un error dentro de ella **lo puede capturar quien
la llama**:

```odio
fn int divide(int a, int b):
    return a / b

get endpoint("/x"):
    try:
        return { "r": divide(1, 0) }
    catch e:
        return { "fallo": e.message }
```

También se pueden usar dentro de un bloque `validate:`:

```odio
class Registro:
    string correo

    validate:
        es_correo(correo)   "correo: formato invalido"
```

Declarar una función con el nombre de un builtin es un error de compilación.

---

## 19. El lenguaje

### Tipos

```
int  long  float  double  bool  string        primitivos, en minúscula
Json  List<T>  Dict<K,V>  File                clases nativas, en mayúscula
```

`T?` marca que el valor puede faltar. Los genéricos son **borrados**: el checker los verifica
y desaparecen antes del bytecode. No hay clases genéricas de usuario.

### Veracidad

Son **falsos** `null`, `false`, `0`, `0.0`, `""`, y la lista y el diccionario vacíos. Es la
regla de Python.

No es coerción: no hay conversión de tipos dentro de los operadores.

```odio
1 + "1"     # error
0 == "0"    # false
"n = " + str(n)     # así se concatena un número
```

Una trampa heredada de Python: con un valor opcional, `if x:` no distingue "vale cero" de
"no venía". Para presencia, `x == null`.

### Sentencias

```odio
int n = 5
n = n + 1

if n > 3:
    ...
else if n > 1:
    ...
else:
    ...

while n > 0:
    n = n - 1

for int x in [1, 2, 3]:
    ...
for string k in miDiccionario:      # recorre las claves
    ...

require n > 0 else status(400)

try:
    int x = n / 0
catch e:
    log.warn(e.message)

break
continue
return valor
```

`for` recorre listas y las claves de un diccionario. `try/catch` se resuelve con una tabla
de rangos calculada al compilar, así que un `return` o un `break` dentro del `try` no dejan
un manejador colgado. El error llega al `catch` como un valor con `message`.

### Expresiones

Precedencia, de menor a mayor: `?:` · `or` · `and` · `not` · `==` `!=` · `<` `<=` `>` `>=` ·
`+` `-` · `*` `/` `%` · `-` unario · `.` `()` `[]`.

```odio
string rol = edad >= 18 ? "adulto" : "menor"
```

---

## 20. Referencia de builtins

### Funciones

| | |
|---|---|
| `text(v)` `html(v)` `json(v)` | Escriben la respuesta |
| `render(plantilla, k=v, ...)` | Renderiza con Jinja2 |
| `status(código)` `redirect(destino[, código])` `send_file(ruta)` | |
| `len(v)` | Tamaño de string, List o Dict |
| `str(v)` `int(v)` | Conversión explícita |
| `header(nombre[, defecto])` | Cabecera de la petición |
| `query(nombre[, defecto])` | Parámetro de query |
| `cookie(nombre[, defecto])` | Cookie de la petición |
| `form(nombre[, defecto])` | Campo de un formulario `urlencoded` |
| `await sleep(ms)` | Suspende |

### Objetos reservados

| Objeto | Miembros | Dónde |
|---|---|---|
| `request` | `path` `method` `ip` | Cualquier handler |
| `session` | cualquier campo, `clear()` | Cualquier handler |
| `jwt` | `valid` `claims` | Cualquier handler |
| `state` | `incr` `decr` `get` `set` `remove` | Cualquier handler |
| `log` | `info` `warn` `error` | En todas partes |
| `sse` | `send` `ping` `open` | Rutas `sse` |
| `ws` | `send` `recv` `open` `close` | Rutas `ws` |
| `error` | `code` `message` `messages` | Bloques `on error` |
| `sqlite` `postgres` `mysql` | `query` `exec` `last_id` `begin` `commit` `rollback` | Con `import` y su bloque en `app:` |

Usar uno fuera de su contexto es error de compilación. Todos los métodos de un módulo de
base de datos son asíncronos: se llaman con `await`.

### Métodos por tipo

| Receptor | Métodos |
|---|---|
| Cualquiera | `status(código)` `header(k, v)` `cookie(k, v, ...)` |
| `string` | `starts_with` `ends_with` `contains` `upper` `lower` `trim` |
| `List` | `add(v)` |
| `Dict` | `has(clave)` `keys()` |
| `File` | `save(directorio)` |

---

## 21. Errores frecuentes

| Mensaje | Qué pasa |
|---|---|
| `el patron declara ':id' pero ningun parametro lo recoge` | Falta el parámetro en la firma |
| `'sleep()' es asincrono: hay que escribir 'await sleep(...)'` | Falta el `await` |
| `'text()' no es asincrono: sobra el 'await'` | Sobra el `await` |
| `'sse' solo existe dentro de una ruta sse` | Objeto reservado fuera de contexto |
| `una ruta ws necesita origins(...)` | Falta la lista blanca de orígenes |
| `no se puede sumar int y string` | Operación entre tipos distintos |
| `la sesion no esta configurada` | Falta `session: secret ...` en `app:` |
| `'X' no esta declarada` | Nombre desconocido, también dentro de `validate` |

Todos salen con fichero, línea, columna y un cursor bajo la posición exacta.

---

## 22. Cómo funciona por dentro

```
osodio ./app  →  lex → parse → check → emitir
                 ↓
              tabla de rutas + bytecode   (una vez, no por petición)
                 ↓
              N hilos: event loop + VM propio, SO_REUSEPORT
```

**Compilación única.** Lexer, parser, análisis semántico y emisión ocurren al arrancar y una
vez por cada cambio de fichero. Nunca por petición.

**Dos niveles.** Las rutas declarativas son entradas en el radix tree con una acción nativa:
cero pasos interpretados. Las demás ejecutan bytecode que solo hace pegamento — el trabajo
real (parseo HTTP, routing, I/O de fichero, plantillas, JSON) siempre es C++ nativo.

**El VM no sabe esperar.** Al llegar a una llamada asíncrona recoge los argumentos y se
detiene; el handler, que ya es una corrutina, hace el `co_await` de verdad sobre el motor y
lo reanuda con el resultado. Por eso el VM tiene pila y locales propios en vez de usar la de
C++: es lo que permite detenerse a mitad.

**Un VM por petición en vuelo**, alojado en el marco de la corrutina del handler. Los chunks
que no pueden suspenderse —y eso se sabe al compilar— reutilizan uno por hilo y se ahorran
las reservas.

**Recarga.** El módulo tiene su propio router; el motor solo lleva una entrada comodín que
delega. Cambiar de versión es publicar un `shared_ptr`: sin `dlopen`, sin `.so`, sin
reiniciar. Si la versión nueva no compila, no se publica.

**Tope de pasos.** Un bucle infinito en un `.odio` corta con un error en vez de clavar un
hilo del event loop, que se llevaría por delante todas las conexiones de ese core. El
contador se reinicia en cada suspensión, para que un bucle de SSE legítimo pueda vivir horas.
