#include <odio/jinja_puente.hpp>

namespace odio {

jinja2::Value valor_plantilla(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null:  return {};
        case Value::Type::Bool:  return v.as_bool();
        case Value::Type::Int:   return static_cast<int64_t>(v.as_int());
        case Value::Type::Float: return v.as_float();
        case Value::Type::Str:   return v.as_str();
        case Value::Type::List: {
            jinja2::ValuesList lista;
            lista.reserve(v.as_list().size());
            for (const auto& item : v.as_list()) lista.push_back(valor_plantilla(item));
            return lista;
        }
        case Value::Type::Dict: {
            jinja2::ValuesMap mapa;
            for (const auto& [k, item] : v.as_dict()) mapa[k] = valor_plantilla(item);
            return mapa;
        }
    }
    return {};
}

jinja2::ValuesMap valores_plantilla(const Value& dict) {
    jinja2::ValuesMap mapa;
    if (!dict.is_dict()) return mapa;
    for (const auto& [k, item] : dict.as_dict()) mapa[k] = valor_plantilla(item);
    return mapa;
}

} // namespace odio
