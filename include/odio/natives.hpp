#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "value.hpp"

namespace osodio {
class Request; class Response; class SSEWriter; class WSConnection;
struct MultipartPart;
}

namespace odio {

// Contexto que el VM pasa a los builtins: es el puente de la capa 2, por donde
// el bytecode alcanza el motor nativo.
// Almacen compartido entre TODOS los hilos de event loop.
//
// Es la unica via de estado comun: cada VM tiene su pila y su heap y no
// comparte nada con los demas.  Por eso expone operaciones y no propiedades —
// `state.x = state.x + 1` seria una carrera entre la lectura y la escritura.
class SharedState {
public:
    static SharedState& instance();

    long long incr(const std::string& key, long long by);
    Value     get(const std::string& key) const;
    void      set(const std::string& key, Value v);
    bool      remove(const std::string& key);

private:
    mutable std::mutex          mutex_;
    std::map<std::string, Value> data_;
};

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

    // Partes multipart ya parseadas.  Un File del lenguaje guarda el indice de
    // su parte aqui, no los bytes: asi copiar un File es copiar un entero.
    const std::vector<osodio::MultipartPart>* parts   = nullptr;
    bool                                      uploads = false;

    // Solo en rutas ws: la conexion ya establecida.
    osodio::WSConnection* ws = nullptr;

    // Sesion: cookie firmada, sin estado en servidor.  Se carga perezosamente
    // en el primer acceso y solo se reescribe si el handler la modifica.
    SessionState* session = nullptr;

    // Solo en un manejador `on error`: el codigo, el motivo, y —cuando el
    // error viene de validar un cuerpo— la lista completa de mensajes.
    int                             error_code    = 0;
    std::string                     error_message;
    const std::vector<std::string>* error_messages = nullptr;

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

// Metodo sobre un valor: "hola".starts_with(...), lista.add(...), fichero.save(...).
// A diferencia de los objetos reservados, el receptor se conoce en runtime, asi
// que el despacho es por tipo dentro del VM.
Value call_method(NativeCtx& ctx, Value& receiver, const std::string& name,
                  std::vector<Value>& args, std::string& error);
bool             is_reserved_object(const std::string& name);

// Mensajes de la ultima validacion fallida en este hilo.  Se rellenan al
// construir el 422 y los lee el manejador `on error` de la misma peticion; no
// hay suspension entre ambos momentos, asi que no pueden cruzarse peticiones.
std::vector<std::string>& last_validation_messages();
const NativeDef& native_at(int id);
int              native_count();

} // namespace odio
