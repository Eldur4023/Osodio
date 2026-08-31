#!/usr/bin/env bash
#
# Suite de regresion de Osodio 2.0.
#
# Levanta el binario contra ficheros .odio reales y comprueba las respuestas.
# Esta escrita en shell a proposito: prueba el binario tal y como se usa, por el
# socket, sin enlazar nada del proyecto.
#
#   tests/run_tests.sh [ruta-al-binario]
#
# Sale con 0 si todo pasa.

set -u

OSODIO="${1:-$HOME/osodio-build/osodio}"
AQUI="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PUERTO=${OSODIO_TEST_PORT:-8790}
SRV=""

pasadas=0
fallidas=0

# ─── Utilidades ──────────────────────────────────────────────────────────────

rojo()  { printf '\033[31m%s\033[0m\n' "$*"; }
verde() { printf '\033[32m%s\033[0m\n' "$*"; }

ok()   { pasadas=$((pasadas + 1)); printf '  ok    %s\n' "$1"; }
fallo() {
    fallidas=$((fallidas + 1))
    rojo "  FALLA $1"
    printf '        esperado: %s\n        obtenido: %s\n' "$2" "$3"
}

# Arranca un servidor con el .odio dado y espera a que responda.
# Cada suite usa su propio puerto: con SO_REUSEPORT dos procesos comparten
# puerto y el kernel reparte conexiones entre ambos, lo que falsearia todo.
levantar() {
    parar
    PUERTO=$((PUERTO + 1))
    "$OSODIO" --no-watch --port "$PUERTO" "$1" > "$TMP/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PUERTO/__ping__" 2>/dev/null; then
            return 0
        fi
        kill -0 "$SRV" 2>/dev/null || { rojo "el servidor murio al arrancar:"; cat "$TMP/srv.log"; return 1; }
        sleep 0.2
    done
    rojo "el servidor no respondio"; cat "$TMP/srv.log"; return 1
}

parar() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    # Esperar SOLO a ese pid: un `wait` sin argumentos esperaria tambien al
    # servidor y colgaria la suite.
    wait "$SRV" 2>/dev/null
    SRV=""
}

# comprueba <nombre> <metodo> <ruta> <codigo-esperado> [subcadena-esperada] [datos]
comprueba() {
    local nombre="$1" metodo="$2" ruta="$3" cod="$4" trozo="${5:-}" datos="${6:-}"
    local args=(-sS --max-time 10 -X "$metodo" -o "$TMP/body" -w '%{http_code}')
    [ -n "$datos" ] && args+=(-H 'Content-Type: application/json' -d "$datos")

    local got
    got=$(curl "${args[@]}" "http://127.0.0.1:$PUERTO$ruta" 2>/dev/null)
    local body
    body=$(cat "$TMP/body" 2>/dev/null)

    if [ "$got" != "$cod" ]; then
        fallo "$nombre" "codigo $cod" "codigo $got — $body"
        return
    fi
    if [ -n "$trozo" ] && ! grep -qF "$trozo" "$TMP/body"; then
        fallo "$nombre" "que contenga '$trozo'" "$body"
        return
    fi
    ok "$nombre"
}

# no_compila <nombre> <fichero> <subcadena-del-error>
no_compila() {
    local nombre="$1" fich="$2" trozo="$3"
    local salida
    salida=$("$OSODIO" --check "$fich" 2>&1)
    if [ $? -eq 0 ]; then
        fallo "$nombre" "error de compilacion" "compilo sin quejarse"
        return
    fi
    if ! printf '%s' "$salida" | grep -qF "$trozo"; then
        fallo "$nombre" "error con '$trozo'" "$(printf '%s' "$salida" | head -2)"
        return
    fi
    ok "$nombre"
}

compila() {
    local nombre="$1" fich="$2"
    if "$OSODIO" --check "$fich" > "$TMP/check" 2>&1; then
        ok "$nombre"
    else
        fallo "$nombre" "que compile" "$(head -3 "$TMP/check")"
    fi
}

limpiar() { parar; rm -rf "$TMP"; }
trap limpiar EXIT

# ─── Suites ──────────────────────────────────────────────────────────────────

echo "== lenguaje =="
levantar "$AQUI/casos/lenguaje.odio" || exit 1
comprueba "aritmetica"          GET /aritmetica       200 '"suma":7'
comprueba "cadenas"             GET /cadenas          200 '"mayus":"HOLA"'
comprueba "multilinea sin margen" GET /multilinea     200 '"sql":"SELECT id\nFROM posts"'
comprueba "multilinea de una"   GET /multilinea       200 '"suelta":"en una linea"'
comprueba "multilinea escapes"  GET /multilinea       200 '"escapes":"con \"comillas\""'
comprueba "veracidad"           GET /veracidad        200 '"cero":false'
comprueba "sin coercion"        GET /coercion         500 'no se puede sumar'
comprueba "condicional elif"    GET /clasifica/0      200 '"r":"cero"'
comprueba "condicional else"    GET /clasifica/99     200 '"r":"grande"'
comprueba "bucle while"         GET /suma_hasta/10    200 '"total":55'
comprueba "bucle for y break"   GET /pares            200 '"pares":[2,4,6]'
comprueba "operadores"          GET /operadores       200 '"a":2'
comprueba "incremento previo"   GET /incremento       200 '"pre":7'
comprueba "indices"             GET /indices          200 '"l":[99,20,25]'
comprueba "ternario"            GET /ternario/20      200 '"rol":"adulto"'
comprueba "try captura"         GET /captura          200 'division por cero'
comprueba "error sin capturar"  GET /revienta         500 'division por cero'

echo "== rutas y parametros =="
levantar "$AQUI/casos/rutas.odio" || exit 1
comprueba "parametro de ruta"   GET /eco/42           200 '"id":42'
comprueba "query con defecto"   GET /pagina           200 '"page":1'
comprueba "query explicita"     GET '/pagina?page=7'  200 '"page":7'
comprueba "tipo invalido"       GET /eco/abc          400 'parametro invalido'
comprueba "ruta inexistente"    GET /nada             404 'no existe'
comprueba "grupo con prefijo"   GET /api/v1/hola      200 '"v":1'
comprueba "guarda deniega"      GET /admin/panel      403
comprueba "guarda permite"      GET '/admin/panel?k=abre' 200 '"panel":true'
comprueba "manejador 404"       GET /tampoco          404 '"ruta":"/tampoco"'

echo "== clases y validacion =="
levantar "$AQUI/casos/clases.odio" || exit 1
comprueba "cuerpo valido"       POST /alta 201 '"creado":"Ana"' '{"nombre":"Ana","edad":30}'
comprueba "campo obligatorio"   POST /alta 422 'edad: obligatorio' '{"nombre":"Ana"}'
comprueba "tipo equivocado"     POST /alta 422 'se esperaba int' '{"nombre":"Ana","edad":"30"}'
comprueba "regla incumplida"    POST /alta 422 'mayor de edad' '{"nombre":"Ana","edad":10}'
comprueba "todos los mensajes"  POST /alta 422 'nombre: obligatorio' '{"nombre":"","edad":10}'
comprueba "json invalido"       POST /alta 400 'JSON invalido' '{roto'
comprueba "mensajes en on error" POST /alta 422 '"cuantos":2' '{"nombre":"","edad":10}'
comprueba "constructor"         GET /punto/3/4        200 '"cuadrado":25'
comprueba "metodo con defecto"  GET /etiqueta/1/2     200 '"otra":"Q(1,2)"'
comprueba "funcion de usuario"  GET /doble/21         200 '"r":42'
comprueba "recursion"           GET /factorial/5      200 '"r":120'
comprueba "tope de recursion"   GET /infinita         500 'demasiada recursion'

echo "== sesion y jwt =="
levantar "$AQUI/casos/sesion.odio" || exit 1
comprueba "sin sesion"          GET /quien            200 '"usuario":null'
comprueba "zona protegida"      GET /admin/panel      403
comprueba "cookie falsificada"  GET /quien            200 '"usuario":null'
comprueba "jwt ausente"         GET /api/yo           401
comprueba "jwt invalido"        GET /api/yo           401

echo "== base de datos =="
rm -f "$TMP/pruebas.db"
levantar "$AQUI/casos/datos.odio" || exit 1
comprueba "crear tabla"         GET  /crear           200 '"ok":true'
comprueba "insertar"            POST /alta/ana        201 '"id":1'
comprueba "insertar otro"       POST /alta/bob        201 '"id":2'
comprueba "listar"              GET  /todos           200 '"nombre":"ana"'
comprueba "buscar por id"       GET  /uno/1           200 '"nombre":"ana"'
comprueba "no encontrado"       GET  /uno/99          404
comprueba "inyeccion sql"       GET  "/busca?q=ana'%20OR%20'1'='1" 200 '"encontrados":0'
comprueba "transaccion"         POST /transfiere      200 '"ok":true'
comprueba "saldos tras commit"  GET  /saldos          200 '"saldo":70'
comprueba "rollback"            POST /deshace         200 '"deshecho":true'
comprueba "saldos tras rollback" GET /saldos          200 '"saldo":70'
comprueba "error del motor"     GET  /malo            200 'no such table'

echo "== errores de compilacion =="
compila    "los ejemplos del repo compilan" "$AQUI/casos/lenguaje.odio"
no_compila "patron sin parametro"  "$AQUI/casos/malos/patron.odio"   "ningun parametro lo recoge"
no_compila "await que falta"       "$AQUI/casos/malos/await.odio"    "es asincrono"
no_compila "objeto fuera de sitio" "$AQUI/casos/malos/sse.odio"      "solo existe dentro de una ruta sse"
no_compila "ws sin origins"        "$AQUI/casos/malos/ws.odio"       "necesita origins"
no_compila "campo inexistente"     "$AQUI/casos/malos/validate.odio" "no esta declarada"
no_compila "metodo inexistente"    "$AQUI/casos/malos/metodo.odio"   "no tiene un metodo"
no_compila "modulo sin importar"   "$AQUI/casos/malos/import.odio"   "falta 'import sqlite'"
# Los tipos de las expresiones se comprueban en EJECUCION, no al compilar:
# el compilador verifica nombres, aridad y contexto, no que `s - 1` cuadre.
# Eso ya lo cubre la prueba "sin coercion" de la suite de lenguaje.

# ─── Resumen ─────────────────────────────────────────────────────────────────

echo
if [ "$fallidas" -eq 0 ]; then
    verde "$pasadas pruebas, todas pasan"
    exit 0
fi
rojo "$pasadas pasan, $fallidas fallan"
exit 1
