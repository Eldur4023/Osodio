#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <algorithm>
#include "validation.hpp"

// ─── std::optional<T> support for nlohmann/json ──────────────────────────────
// nlohmann v3.11 doesn't ship optional serialization by default.
// null JSON → std::nullopt; any other value → T.
namespace nlohmann {
template<typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json& j, const std::optional<T>& opt) {
        if (opt) j = *opt;
        else     j = nullptr;
    }
    static void from_json(const json& j, std::optional<T>& opt) {
        if (j.is_null()) opt = std::nullopt;
        else             opt = j.get<T>();
    }
};
} // namespace nlohmann

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

// ─── SchemaFields<T> ─────────────────────────────────────────────────────────
// All field names registered by OSODIO_SCHEMA (required + optional).

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

// ─── OptionalFields<T> ───────────────────────────────────────────────────────
// Fields listed in OSODIO_OPTIONAL — absent from request body is allowed.

template<typename T, typename = void>
struct OptionalFields {
    static const std::vector<std::string>& names() {
        static const std::vector<std::string> empty;
        return empty;
    }
    static bool contains(const std::string&) { return false; }
};
template<typename T>
struct OptionalFields<T, std::void_t<decltype(T::_optional_fields())>> {
    static const std::vector<std::string>& names() {
        return T::_optional_fields();
    }
    static bool contains(const std::string& field) {
        const auto& n = names();
        return std::find(n.begin(), n.end(), field) != n.end();
    }
};

} // namespace osodio

// ─── OSODIO_SCHEMA ────────────────────────────────────────────────────────────
// Genera from_json / to_json y registra nombres de campos.
// Uso: DENTRO del struct, después de declarar los campos.
//
// Usa NLOHMANN_DEFINE_TYPE_INTRUSIVE (estricto). Los campos opcionales ausentes
// en el body JSON se inyectan como null antes de la deserialización en
// extract_body — nlohmann convierte null → nullopt para std::optional<T>.
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

// ─── OSODIO_OPTIONAL ──────────────────────────────────────────────────────────
// Marca campos cuya ausencia en el body JSON está permitida.
// Uso: DENTRO del struct, después de OSODIO_SCHEMA.
// Los campos deben ser std::optional<T> en C++.
//
//   struct UpdateUser {
//       std::optional<std::string> name;
//       std::optional<int> age;
//       OSODIO_SCHEMA(UpdateUser, name, age)
//       OSODIO_OPTIONAL(name, age)
//   };
//
#define OSODIO_OPTIONAL(...)                                                  \
    static const std::vector<std::string>& _optional_fields() {             \
        static const auto _n =                                               \
            osodio::detail::split_field_names(#__VA_ARGS__);                \
        return _n;                                                           \
    }

// ─── OSODIO_VALIDATE ─────────────────────────────────────────────────────────
// Define reglas de validación de negocio.
// Uso: DENTRO del struct. No repite el nombre del tipo.
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

// check(condition, message) — usado dentro de OSODIO_VALIDATE.
#define check(cond, msg) \
    ((cond) ? (void)0 : _errors.push_back(msg))
