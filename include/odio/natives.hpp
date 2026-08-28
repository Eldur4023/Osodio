#pragma once
#include <string>
#include <vector>

#include "value.hpp"

namespace osodio { class Request; class Response; class SSEWriter; class WSConnection; }

namespace odio {

// Contexto que el VM pasa a los builtins: es el puente de la capa 2, por donde
// el bytecode alcanza el motor nativo.
// Estado de la sesion durante una peticion.
struct SessionState {
    Value::Dict data;
    bool        loaded = false;   // ya se intento leer la cookie
    bool        dirty  = false;   // el handler la modifico: hay que reescribirla
    std::string secret;           // vacio = sesiones no configuradas
};

struct NativeCtx {
    osodio::Request&  req;
    osodio::Response& res;

    // Solo en rutas sse: el escritor del flujo, creado por el driver antes de
    // arrancar el VM.  Nulo en el resto, y los builtins de sse lo comprueban.
    osodio::SSEWriter* sse = nullptr;

    // Solo en rutas ws: la conexion ya establecida.
    osodio::WSConnection* ws = nullptr;

    // Sesion: cookie firmada, sin estado en servidor.  Se carga perezosamente
    // en el primer acceso y solo se reescribe si el handler la modifica.
    SessionState* session = nullptr;

    // Solo en un manejador `on error`: el codigo y el motivo.
    int         error_code    = 0;
    std::string error_message;

    // Claims del JWT del Authorization: Bearer, ya verificados por el driver.
    const Value* jwt_claims = nullptr;
    bool         jwt_ok     = false;

    // Lo pone cualquier builtin que escriba la respuesta (text, render,
    // redirect...).  Si al terminar el handler sigue en false, el valor
    // devuelto se serializa como JSON.
    bool response_written = false;
};

// Un builtin devuelve un Value y, si falla, escribe el motivo en `error`.
using NativeFn = Value (*)(NativeCtx& ctx, std::vector<Value>& args,
                           std::string& error);

struct NativeDef {
    const char* name;
    int         min_args;
    int         max_args;    // -1 = sin limite
    NativeFn    fn;          // nulo si es asincrono
    bool        is_async = false;
};

// Builtins que suspenden.  El VM no los ejecuta: los devuelve al driver, que
// es quien puede hacer un co_await de verdad sobre el motor.  El identificador
// es el mismo indice de la tabla, asi que se consulta por nombre una sola vez.

// Indice estable del builtin en la tabla, o -1 si no existe.  El emisor guarda
// el indice en la instruccion, asi que el VM no busca por nombre en runtime.
int              native_id(const std::string& name);

// Resuelve `objeto.miembro` de un objeto reservado (sse.send, sse.open...) al
// builtin que lo implementa, o -1 si esa combinacion no existe.  El mapeo vive
// junto a la tabla para que anadir un miembro sea tocar un sitio.
int              member_native_id(const std::string& object, const std::string& member);
bool             is_reserved_object(const std::string& name);
const NativeDef& native_at(int id);
int              native_count();

} // namespace odio
