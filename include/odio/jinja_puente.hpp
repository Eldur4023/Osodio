#pragma once
#include <jinja2cpp/value.h>

#include "value.hpp"

namespace odio {

// Puente de un Value de Odio a los valores que consume Jinja2Cpp.
//
// Antes este camino pasaba por un arbol nlohmann intermedio: Value -> json ->
// jinja2.  Dos conversiones para ir de A a C, y la del medio no la queria
// nadie.
jinja2::Value       valor_plantilla(const Value& v);
jinja2::ValuesMap   valores_plantilla(const Value& dict);

} // namespace odio
