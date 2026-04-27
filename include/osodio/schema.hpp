#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <optional>
#include <type_traits>
#if __cpp_reflection
#include <ranges>
#endif

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
inline void bind_field(const nlohmann::json& j, const std::string& name, FieldType& field) {
    if constexpr (is_optional_v<FieldType>) {
        if (j.contains(name) && !j.at(name).is_null())
            j.at(name).get_to(field);
        else
            field = std::nullopt;
    } else {
        j.at(name).get_to(field);
    }
}

} // namespace osodio::detail

#if __cpp_reflection
// ─── Universal aggregate serialization via C++26 P2996 static reflection ─────
//
// Provides to_json / from_json automatically for any aggregate struct.
// No SCHEMA macro needed — just declare your fields and they're serialized.
//
// Excluded: raw arrays, ranges (std::array, std::vector, etc.), nlohmann::json,
// and std::optional<T> (handled by its own adl_serializer above).

namespace nlohmann {

template<typename T>
    requires (std::is_aggregate_v<T>
              && !std::is_array_v<T>
              && !std::ranges::range<T>
              && !std::is_same_v<std::remove_cv_t<T>, json>
              && !::osodio::detail::is_optional_v<T>)
struct adl_serializer<T> {
    static void to_json(json& j, const T& t) {
        j = json::object();
        template for (constexpr auto m : std::meta::members_of(^^T)) {
            j[std::string(std::meta::identifier_of(m))] = t.[:m:];
        }
    }
    static void from_json(const json& j, T& t) {
        template for (constexpr auto m : std::meta::members_of(^^T)) {
            ::osodio::detail::bind_field(j, std::string(std::meta::identifier_of(m)), t.[:m:]);
        }
    }
};

} // namespace nlohmann
#endif // __cpp_reflection

// ─── OSODIO_FROM_FIELD_ ───────────────────────────────────────────────────────
// Delegates to bind_field so if constexpr works inside a non-template from_json.

#define OSODIO_FROM_FIELD_(field) \
    ::osodio::detail::bind_field(nlohmann_json_j, std::string(#field), nlohmann_json_t.field);

// ─── SCHEMA ───────────────────────────────────────────────────────────────────
//
// Adds JSON serialization to a struct. Place it inside the struct body after
// all field declarations.
//
//   struct User {
//       std::string name;
//       std::optional<std::string> bio;
//       int age;
//       SCHEMA(User, name, bio, age)   // ← delete this when on C++26
//   };
//
// With C++26 reflection active (__cpp_reflection), SCHEMA becomes a no-op:
// to_json / from_json are generated automatically for any aggregate struct.
// Migration path: just delete SCHEMA(...) from each struct.

#if __cpp_reflection

#define SCHEMA(Type, ...)     // reflection active — auto-generated, safe to delete
#define OSODIO_SCHEMA SCHEMA

#else

#define SCHEMA(Type, ...)                                                       \
    friend void to_json(nlohmann::json& nlohmann_json_j,                       \
                        const Type& nlohmann_json_t) {                         \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) \
    }                                                                           \
    friend void from_json(const nlohmann::json& nlohmann_json_j,               \
                          Type& nlohmann_json_t) {                             \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(OSODIO_FROM_FIELD_, __VA_ARGS__)) \
    }

// Backward-compat alias.
#define OSODIO_SCHEMA SCHEMA

#endif // __cpp_reflection
