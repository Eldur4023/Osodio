# Odio — Gramática

> Gramática formal del lenguaje que interpreta Osodio 2.0. Notación: `::=` producción,
> `|` alternativa, `[ ]` opcional, `{ }` cero o más, `( )` agrupación, MAYÚSCULAS terminal
> del lexer, `"texto"` literal.

---

## 1. Léxico

### Comentarios
```
# hasta el final de la línea
```

### Bloques por indentación
El lexer emite `INDENT` y `DEDENT` como Python. Un bloque es una secuencia de sentencias con
mayor indentación que la línea del encabezado. Espacios, no tabuladores.

### Palabras reservadas

```
import  class  fn  app  group  endpoint  on  error  origins
get  post  put  patch  delete  any  sse  ws
if  else  while  for  in  return  require  try  catch  break  continue
validate  and  or  not  true  false  null  this  void  spa
```

### Objetos reservados
Existen sin declararse, solo dentro del contexto que los define.

| Objeto | Disponible en |
|---|---|
| `request` | cualquier handler |
| `session` | cualquier handler |
| `state` | cualquier handler y función |
| `jwt` | handlers bajo una guarda `jwt` |
| `sse` | handlers `sse` |
| `ws` | handlers `ws` |
| `error` | bloques `on error` |
| `log` | en todas partes |
| `this` | métodos y constructores |

### Literales
```
INT      ::= dígito { dígito }
FLOAT    ::= dígito { dígito } "." dígito { dígito }
STRING   ::= '"' { carácter } '"'
BOOL     ::= "true" | "false"
NULL     ::= "null"
IDENT    ::= ( letra | "_" ) { letra | dígito | "_" }
```

`letra` incluye Unicode: `contraseña` es un identificador válido.

### Nota de lexado: `>>`
El lexer **no fusiona** `>` `>` en un token de desplazamiento. `List<Dict<string,int>>`
cierra con dos `>` independientes. C++ arrastró este bug hasta C++11; aquí se evita no
creando nunca el token.

---

## 2. Tipos

```
type          ::= base_type [ "?" ]
base_type     ::= primitive | generic_type | IDENT
primitive     ::= "int" | "long" | "float" | "double" | "bool" | "string" | "void"
generic_type  ::= IDENT "<" type { "," type } ">"
```

`T?` marca que el valor puede faltar. Un campo no opcional ausente en el body es un 422
automático.

### Clases nativas

| Tipo | Qué es |
|---|---|
| `Json` | Datos dinámicos: body sin esquema, mensaje WS, literal de respuesta |
| `List<T>` | Lista homogénea |
| `Dict<K,V>` | Mapa homogéneo |
| `File` | Fichero subido: `name`, `filename`, `content_type`, `size`, `save(dir)` |

Los genéricos son **borrados**: el checker los verifica y desaparecen antes del bytecode.
El VM nunca ve un `List<File>`, solo una lista.

**No hay clases genéricas de usuario en 2.0.** `class Caja<T>` no es válido. Es aditivo:
puede llegar después sin romper nada.

### `?` en tipo frente a ternario
`string? x = a ? b : c` es válido y no ambiguo, pero exige un token de lookahead: tras un
`?` en posición de declaración viene un `IDENT`; en un ternario viene una expresión seguida
de `:`.

---

## 3. Programa

```
program       ::= { top_level }

top_level     ::= import_decl
                | class_decl
                | fn_decl
                | app_decl
                | group_decl
                | route_decl
                | error_decl
```

El orden entre declaraciones y entre ficheros es indiferente: la compilación es en dos
pasadas, primero se recogen las declaraciones y después se resuelven los nombres.

---

## 4. Declaraciones

### import
```
import_decl   ::= "import" IDENT NEWLINE
```
Introduce un espacio de nombres. Los módulos (`SQLite`, `Postgres`, `MySQL`) quedan fuera
del alcance de 2.0; la sintaxis existe para no cerrarles la puerta.

### class
```
class_decl    ::= "class" IDENT ":" INDENT { class_member } DEDENT

class_member  ::= field | validate_block | constructor

field         ::= type IDENT NEWLINE

validate_block ::= "validate" ":" INDENT { validate_rule } DEDENT
validate_rule  ::= expr STRING NEWLINE

constructor   ::= IDENT "(" [ params ] ")" ( ":" block | NEWLINE )
```

Un constructor **sin cuerpo** asigna los parámetros a los campos del mismo nombre, en orden.
Solo pueden omitirse campos opcionales.

`validate_rule` es una expresión booleana y el mensaje que se emite si es falsa. Si alguna
falla, el handler no llega a ejecutarse y la respuesta es 422 con la lista de mensajes.

### fn
```
fn_decl       ::= "fn" type IDENT "(" [ params ] ")" ":" block
params        ::= param { "," param }
param         ::= type IDENT [ "=" expr ]
```

### app
```
app_decl      ::= "app" ":" INDENT { app_entry } DEDENT

app_entry     ::= "name"      STRING
                | "version"   STRING
                | "port"      INT
                | "templates" STRING
                | "static"    STRING "->" STRING [ "spa" ]
                | "docs" | "health" | "metrics"
                | IDENT ":" INDENT { config_pair } DEDENT

config_pair   ::= IDENT expr NEWLINE
```

`app:` puede aparecer en cualquier fichero del proyecto, pero **solo una vez**.

### Rutas
```
route_decl    ::= method "endpoint" "(" STRING { "," param } ")"
                  [ modifier ] ":" block

method        ::= "get" | "post" | "put" | "patch" | "delete" | "any"
                | "sse" | "ws"

modifier      ::= "origins" "(" STRING { "," STRING } ")"
```

El primer `STRING` es el patrón de ruta (`:param`, `{param}`, `*`). Los `param` que
coinciden con un segmento de la ruta se enlazan a él; el resto se resuelven como query, body
o parte multipart según su tipo. **El checker verifica que cada `:nombre` del patrón tiene un
parámetro que lo recoge, y al revés.**

`sse` implica GET. `origins` solo es válido en `ws`, y en `ws` es **obligatorio**: la
gramática lo admite en cualquier ruta y el checker lo rechaza fuera de sitio.

### group
```
group_decl    ::= "group" "(" STRING ")" ":" INDENT { group_member } DEDENT
group_member  ::= require_stmt | route_decl | group_decl
```
Los grupos anidan y las guardas se acumulan.

### on error
```
error_decl    ::= "on" "error" [ INT ] ":" block
```
Sin código, es el handler global. El código, si se da, tiene que estar entre **400 y 599**:
con un 2xx el handler de la ruta ya ha escrito la respuesta, y sustituirla sería un filtro
de respuesta — es decir, middleware, que es justo lo que Osodio 2.0 delega al proxy.

Dentro del bloque existe el objeto reservado `error` (`error.code`, `error.message`), y el
manejador puede sustituir el cuerpo por defecto. El código de estado se conserva.

---

## 5. Sentencias

```
block         ::= INDENT { statement } DEDENT

statement     ::= var_decl
                | assign_stmt
                | if_stmt
                | while_stmt
                | for_stmt
                | return_stmt
                | require_stmt
                | try_stmt
                | "break"    NEWLINE
                | "continue" NEWLINE
                | expr NEWLINE

var_decl      ::= type IDENT [ "=" expr ] NEWLINE
assign_stmt   ::= lvalue "=" expr NEWLINE
lvalue        ::= IDENT { "." IDENT | "[" expr "]" }

if_stmt       ::= "if" expr ":" block
                  { "else" "if" expr ":" block }
                  [ "else" ":" block ]

while_stmt    ::= "while" expr ":" block
for_stmt      ::= "for" type IDENT "in" expr ":" block

return_stmt   ::= "return" [ expr ] NEWLINE
require_stmt  ::= "require" expr "else" expr NEWLINE

try_stmt      ::= "try" ":" block "catch" [ IDENT ] ":" block
```

`require X else Y` es azúcar de `if not X: return Y`. No es un constructo de middleware: es
un retorno anticipado, y por eso funciona en cualquier posición, bucles incluidos.

---

## 6. Expresiones

```
expr          ::= ternary
ternary       ::= or_expr [ "?" expr ":" expr ]
or_expr       ::= and_expr { "or" and_expr }
and_expr      ::= not_expr { "and" not_expr }
not_expr      ::= [ "not" ] equality
equality      ::= comparison { ( "==" | "!=" ) comparison }
comparison    ::= sum { ( "<" | "<=" | ">" | ">=" ) sum }
sum           ::= product { ( "+" | "-" ) product }
product       ::= unary { ( "*" | "/" | "%" ) unary }
unary         ::= [ "-" ] postfix
postfix       ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]" }

primary       ::= INT | FLOAT | STRING | BOOL | NULL
                | IDENT
                | "this"
                | "await" expr
                | dict_literal
                | list_literal
                | "(" expr ")"

args          ::= arg { "," arg }
arg           ::= expr | IDENT "=" expr

dict_literal  ::= "{" [ dict_entry { "," dict_entry } ] "}"
dict_entry    ::= expr ":" expr
list_literal  ::= "[" [ expr { "," expr } ] "]"
```

Los argumentos con nombre van después de los posicionales.

### Precedencia, de menor a mayor
```
1.  ?:                    (asociativo a la derecha)
2.  or
3.  and
4.  not                   (unario)
5.  ==  !=
6.  <  <=  >  >=
7.  +  -
8.  *  /  %
9.  -                     (unario)
10. .  ()  []             (postfijo)
```

### Veracidad

En contexto booleano (`if`, `while`, `require`, `and`, `or`, `not`, ternario) son
**falsos**: `null`, `false`, `0`, `0.0`, la cadena vacia, y la lista y el diccionario
vacios. Todo lo demas es verdadero. Es la regla de Python, que es de donde viene la gente
que va a escribir Odio.

No es coercion: no hay conversion de tipos dentro de los operadores. `0 == "0"` es **falso**
en Odio, y `"1" + 1` es un error, no `"11"`.

**Cuidado con una trampa heredada de Python:** con un valor opcional, `if x:` no distingue
"vale cero" de "no venia". Para preguntar por presencia hay que escribir `x == null`.

### await
`await` solo es válido dentro de un handler, sobre una expresión suspendible. En 2.0 son
`sleep(ms)` y `ws.recv()`. Es el motivo de que el VM tenga pila propia: un intérprete que
recursa sobre la pila de C++ no puede suspenderse.

### Inferencia bidireccional
El tipo esperado baja hacia los literales: en `List<string> xs = []`, el `[]` toma su tipo
del lado izquierdo. Sin esto habría que escribir `List<string> xs = List<string>()`, que es
la ceremonia que el lenguaje quiere evitar.

---

## 7. Retorno de un handler

Todo sale por `return`. No hay objeto `response` mutable que arrastrar.

| Forma | Resultado |
|---|---|
| `return <clase>` | 200, cuerpo JSON serializado |
| `return { ... }` | 200, cuerpo JSON |
| `return render("x.html", k=v)` | 200, HTML renderizado con Jinja2 |
| `return send_file(ruta)` | Fichero, zero-copy `sendfile(2)` |
| `return redirect(ruta [, código])` | 302 por defecto |
| `return status(código)` | Código sin cuerpo |

Todas admiten encadenar `.status(código)`, `.header(nombre, valor)` y
`.cookie(nombre, valor, ...)`.

---

## 8. Ambigüedades conocidas

| Caso | Resolución |
|---|---|
| `>>` al cerrar genéricos anidados | El lexer nunca fusiona `>` `>` |
| `?` de tipo opcional frente a ternario | Un token de lookahead: tras `?`, un `IDENT` es declaración |
| `{` de bloque frente a literal de diccionario | Los bloques son por indentación; `{` siempre abre un diccionario |
| `else` colgante | Imposible: los bloques son por indentación |

---

## 9. Sin decidir

- **`else if` frente a `elif`.** La gramática recoge `else if`. Con bloques por indentación
  lo natural sería `elif`; con tipos de aire C, `else if`. Es un cambio de una línea.
- **Historia de tests.** Si Odio tiene forma de testear endpoints en proceso, necesita
  sintaxis propia y esta gramática crece.
