#pragma once
#include <string>
#include <vector>

#include "value.hpp"

namespace osodio { class Request; class Response; }

namespace odio {

// Contexto que el VM pasa a los builtins: es el puente de la capa 2, por donde
// el bytecode alcanza el motor nativo.
struct NativeCtx {
    osodio::Request&  req;
    osodio::Response& res;

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
    NativeFn    fn;
};

// Indice estable del builtin en la tabla, o -1 si no existe.  El emisor guarda
// el indice en la instruccion, asi que el VM no busca por nombre en runtime.
int              native_id(const std::string& name);
const NativeDef& native_at(int id);
int              native_count();

} // namespace odio
