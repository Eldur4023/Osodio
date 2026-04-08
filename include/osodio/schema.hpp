#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <string_view>
#include "validation.hpp"

// ─── SchemaFields<T> ─────────────────────────────────────────────────────────
// Compile-time field name registry populated by OSODIO_SCHEMA.
// extractor<Body<T>> uses this to produce per-field 422 errors.

namespace osodio {

namespace detail {
    inline std::vector<std::string> split_field_names(std::string_view raw) {
        std::vector<std::string> names;
        size_t start = 0;
        while (start < raw.size()) {
            while (start < raw.size() &&
                   (raw[start] == ' ' || raw[start] == '\t')) ++start;
            size_t comma = raw.find(',', start);
            size_t end   = (comma == std::string_view::npos) ? raw.size() : comma;
            size_t field_end = end;
            while (field_end > start &&
                   (raw[field_end-1] == ' ' || raw[field_end-1] == '\t')) --field_end;
            if (field_end > start)
                names.emplace_back(raw.substr(start, field_end - start));
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return names;
    }
} // namespace detail

// Primary template: empty (types without OSODIO_SCHEMA have no field list).
// Types with OSODIO_SCHEMA expose a static _schema_fields() that we detect.
template<typename T, typename = void>
struct SchemaFields {
    static const std::vector<std::string>& names() {
        static const std::vector<std::string> empty;
        return empty;
    }
};
template<typename T>
struct SchemaFields<T, std::void_t<decltype(T::_schema_fields())>> {
    static const std::vector<std::string>& names() {
        return T::_schema_fields();
    }
};

} // namespace osodio

// ─── OSODIO_SCHEMA ────────────────────────────────────────────────────────────
// Genera from_json / to_json y registra nombres de campos.
// Uso: DENTRO del struct, después de declarar los campos.
//
//   struct User {
//       std::string name;
//       int age;
//       OSODIO_SCHEMA(User, name, age)
//   };
//
#define OSODIO_SCHEMA(Type, ...)                                              \
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Type, __VA_ARGS__)                         \
    static const std::vector<std::string>& _schema_fields() {               \
        static const auto _n =                                               \
            osodio::detail::split_field_names(#__VA_ARGS__);                \
        return _n;                                                           \
    }

// ─── OSODIO_VALIDATE ─────────────────────────────────────────────────────────
// Define reglas de validación de negocio.
// Uso: DENTRO del struct, después de OSODIO_SCHEMA.
// No repite el nombre del tipo.
//
//   struct User {
//       std::string name; int age;
//       OSODIO_SCHEMA(User, name, age)
//       OSODIO_VALIDATE(
//           check(name.size() > 0, "Name cannot be empty"),
//           check(age >= 18,       "Must be at least 18")
//       )
//   };
//
#define OSODIO_VALIDATE(...)                                             \
    void _validate_impl(std::vector<std::string>& _errors) const {     \
        __VA_ARGS__;                                                    \
    }

// check(condition, message)
// Se usa dentro de OSODIO_VALIDATE. Accede a los campos de `this` directamente.
#define check(cond, msg) \
    ((cond) ? (void)0 : _errors.push_back(msg))
