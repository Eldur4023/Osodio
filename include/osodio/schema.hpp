#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <optional>
#include <type_traits>

// ─── std::optional<T> support for nlohmann ───────────────────────────────────

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

namespace osodio::detail {

template<typename T> struct is_optional                   : std::false_type {};
template<typename T> struct is_optional<std::optional<T>> : std::true_type  {};
template<typename T>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

// bind_field<FieldType> — used by the SCHEMA macro's from_json.
// Lives in a template so if constexpr properly discards the false branch.
//   - std::optional<T> fields: absent or null JSON → std::nullopt
//   - Required fields:         absent JSON          → nlohmann throws → 422
template<typename FieldType>
inline void bind_field(const nlohmann::json& j, const char* name, FieldType& field) {
    if constexpr (is_optional_v<FieldType>) {
        if (j.contains(name) && !j[name].is_null())
            j.at(name).get_to(field);
        else
            field = std::nullopt;
    } else {
        j.at(name).get_to(field);
    }
}

} // namespace osodio::detail

// ─── OSODIO_FROM_FIELD_ ───────────────────────────────────────────────────────
// Delegates to bind_field so if constexpr works inside a non-template from_json.

#define OSODIO_FROM_FIELD_(field) \
    ::osodio::detail::bind_field(nlohmann_json_j, #field, nlohmann_json_t.field);

// ─── SCHEMA ───────────────────────────────────────────────────────────────────
//
// Adds JSON serialization to a struct. Place it inside the struct body after
// all field declarations.
//
//   struct User {
//       std::string name;
//       std::optional<std::string> bio;   // ← automatically optional, no extra macro
//       int age;
//       SCHEMA(User, name, bio, age)
//
//       // Optional: define validate() for business-rule errors.
//       std::vector<std::string> validate() const {
//           if (name.empty()) return {"name: cannot be empty"};
//           if (age < 18)     return {"age: must be at least 18"};
//           return {};
//       }
//   };
//
// ── C++26 note ────────────────────────────────────────────────────────────────
// Once GCC 15 / Clang 20 ship C++26 static reflection (P2996), SCHEMA will be
// replaced by a universal to_json/from_json that works for any aggregate struct
// with zero macros.  The migration will be: delete SCHEMA(...) from every struct.

#define SCHEMA(Type, ...)                                                       \
    friend void to_json(nlohmann::json& nlohmann_json_j,                       \
                        const Type& nlohmann_json_t) {                         \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) \
    }                                                                           \
    friend void from_json(const nlohmann::json& nlohmann_json_j,               \
                          Type& nlohmann_json_t) {                             \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(OSODIO_FROM_FIELD_, __VA_ARGS__)) \
    }

// Backward-compat alias — migrate to SCHEMA when convenient.
#define OSODIO_SCHEMA SCHEMA
