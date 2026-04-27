#pragma once
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <type_traits>
#include <nlohmann/json.hpp>
#include "handler_traits.hpp"  // PathParam, Body, Query, fixed_string, is_task
#if __cpp_reflection
#include <ranges>
#endif

namespace osodio {


// ─── schema_from_type<T> ─────────────────────────────────────────────────────
// Generates a JSON Schema object from a C++ type.
//
// Strategy:
//   1. Primitives   → {"type": "boolean"/"integer"/"number"/"string"}
//   2. nlohmann::json → {} (opaque — any object)
//   3. Class with OSODIO_SCHEMA → default-construct, serialize to JSON,
//      then infer property types from the serialized values.
//      std::optional<T> fields appear as null → marked {nullable: true}.
//   4. Anything else → {} (opaque)

template<typename T>
nlohmann::json schema_from_type() {
    if constexpr (std::is_same_v<T, nlohmann::json>) {
        return nlohmann::json::object();

    } else if constexpr (std::is_same_v<T, bool>) {
        return {{"type", "boolean"}};

    } else if constexpr (std::is_integral_v<T>) {
        return {{"type", "integer"}};

    } else if constexpr (std::is_floating_point_v<T>) {
        return {{"type", "number"}};

    } else if constexpr (std::is_same_v<T, std::string>) {
        return {{"type", "string"}};

    } else if constexpr (detail::is_optional_v<T>) {
        // optional<T> → inner type schema + nullable:true.
        // This must be checked before the aggregate/class branches so the inner
        // type is preserved instead of being erased to just {"nullable": true}.
        using Inner = typename T::value_type;
        auto inner = schema_from_type<Inner>();
        inner["nullable"] = true;
        return inner;

#if __cpp_reflection
    } else if constexpr (std::is_aggregate_v<T>
                         && !std::is_array_v<T>
                         && !std::ranges::range<T>) {
        nlohmann::json props    = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();
        template for (constexpr auto m : std::meta::members_of(^^T)) {
            using FieldType = [: std::meta::type_of(m) :];
            constexpr auto field_name = std::meta::identifier_of(m);
            props[std::string(field_name)] = schema_from_type<FieldType>();
            if constexpr (!detail::is_optional_v<FieldType>)
                required.push_back(std::string(field_name));
        }
        nlohmann::json schema = {{"type", "object"}, {"properties", props}};
        if (!required.empty()) schema["required"] = required;
        return schema;

#else
    } else if constexpr (std::is_class_v<T> && has_to_json<T>::value) {
        try {
            T defaults{};
            nlohmann::json sample = defaults;
            if (!sample.is_object()) return nlohmann::json::object();

            nlohmann::json props      = nlohmann::json::object();
            nlohmann::json req_fields = nlohmann::json::array();

            for (auto& [k, v] : sample.items()) {
                nlohmann::json prop;
                if      (v.is_null())             prop = {{"nullable", true}};
                else if (v.is_boolean())          prop = {{"type", "boolean"}};
                else if (v.is_number_integer())   prop = {{"type", "integer"}};
                else if (v.is_number_float())     prop = {{"type", "number"}};
                else if (v.is_string())           prop = {{"type", "string"}};
                else if (v.is_array())            prop = {{"type", "array"}, {"items", {}}};
                else                              prop = {{"type", "object"}};

                props[k] = prop;
                if (!v.is_null()) req_fields.push_back(k);
            }

            nlohmann::json schema = {{"type", "object"}, {"properties", props}};
            if (!req_fields.empty()) schema["required"] = req_fields;
            return schema;
        } catch (...) {
            return nlohmann::json::object();
        }

#endif // __cpp_reflection

    } else {
        return nlohmann::json::object();
    }
}

// Unwrap Task<T> to get the actual response value type, then call schema_from_type.
template<typename R>
nlohmann::json response_schema_for() {
    if constexpr (is_task<R>::value) {
        using V = typename R::promise_type::value_type;
        if constexpr (std::is_void_v<V>) return {};
        else return schema_from_type<V>();
    } else if constexpr (std::is_void_v<R>) {
        return {};
    } else {
        return schema_from_type<R>();
    }
}

// ─── Type → OpenAPI primitive name ───────────────────────────────────────────

template<typename T>
constexpr const char* cpp_to_openapi_type() {
    if constexpr (std::is_same_v<T, bool>)             return "boolean";
    else if constexpr (std::is_integral_v<T>)          return "integer";
    else if constexpr (std::is_floating_point_v<T>)    return "number";
    else if constexpr (std::is_same_v<T, std::string>) return "string";
    else                                                return "object";
}

// ─── RouteDoc ─────────────────────────────────────────────────────────────────
// Metadata captured at route registration time (compile-time type erasure).

struct RouteDoc {
    std::string method;
    std::string path;                           // original path ("/users/:id")
    std::optional<nlohmann::json> request_body; // Body<T> schema, if any
    nlohmann::json response_schema;             // return type schema

    struct Param {
        std::string name;
        std::string in;       // "path" | "query"
        std::string type;     // openapi type string
        bool        required;
    };
    std::vector<Param> params;
};

// ─── ArgVisitor ───────────────────────────────────────────────────────────────
// Per-arg-type visitor that populates a RouteDoc.

template<typename Arg>
struct ArgVisitor { static void visit(RouteDoc&) {} }; // default: no-op

template<typename T, fixed_string Name>
struct ArgVisitor<PathParam<T, Name>> {
    static void visit(RouteDoc& doc) {
        doc.params.push_back({
            std::string(std::string_view(Name)),
            "path",
            cpp_to_openapi_type<T>(),
            true
        });
    }
};

template<typename T, fixed_string Name>
struct ArgVisitor<Query<T, Name>> {
    static void visit(RouteDoc& doc) {
        doc.params.push_back({
            std::string(std::string_view(Name)),
            "query",
            cpp_to_openapi_type<T>(),
            false
        });
    }
};

template<typename T>
struct ArgVisitor<Body<T>> {
    static void visit(RouteDoc& doc) {
        doc.request_body = schema_from_type<T>();
    }
};

// ─── DocBuilder ───────────────────────────────────────────────────────────────
// Statically extracts a RouteDoc from a handler's type signature.
// Handles: const lambdas, mutable lambdas, functors.
// TODO: extend for free functions if needed.

template<typename F>
struct DocBuilder : DocBuilder<decltype(&F::operator())> {};

template<typename R, typename C, typename... Args>
struct DocBuilder<R (C::*)(Args...) const> {
    static RouteDoc build(const std::string& method, const std::string& path) {
        RouteDoc doc;
        doc.method          = method;
        doc.path            = path;
        (ArgVisitor<std::decay_t<Args>>::visit(doc), ...);  // fold: visit each arg
        doc.response_schema = response_schema_for<R>();
        return doc;
    }
};

template<typename R, typename C, typename... Args>
struct DocBuilder<R (C::*)(Args...)> {
    static RouteDoc build(const std::string& method, const std::string& path) {
        return DocBuilder<R (C::*)(Args...) const>::build(method, path);
    }
};

// ─── Path conversion: :param → {param} ───────────────────────────────────────
// OpenAPI 3.0 uses {param} syntax; Osodio accepts :param syntax.

inline std::string to_openapi_path(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 4);
    for (size_t i = 0; i < path.size(); ) {
        if (path[i] == ':') {
            out += '{';
            for (++i; i < path.size() && path[i] != '/'; ++i) out += path[i];
            out += '}';
        } else {
            out += path[i++];
        }
    }
    return out;
}

// ─── OpenAPI 3.0 document builder ────────────────────────────────────────────

inline nlohmann::json build_openapi_doc(
    const std::string& title,
    const std::string& version,
    const std::vector<RouteDoc>& routes)
{
    nlohmann::json doc = {
        {"openapi", "3.0.3"},
        {"info",    {{"title", title}, {"version", version}}},
        {"paths",   nlohmann::json::object()}
    };

    for (const auto& route : routes) {
        std::string oa_path = to_openapi_path(route.path);
        std::string method  = route.method;
        std::transform(method.begin(), method.end(), method.begin(), ::tolower);

        nlohmann::json op = nlohmann::json::object();

        // Path and query parameters
        if (!route.params.empty()) {
            nlohmann::json params_arr = nlohmann::json::array();
            for (const auto& p : route.params) {
                params_arr.push_back({
                    {"name",     p.name},
                    {"in",       p.in},
                    {"required", p.required},
                    {"schema",   {{"type", p.type}}}
                });
            }
            op["parameters"] = params_arr;
        }

        // Request body (from Body<T>)
        if (route.request_body.has_value()) {
            op["requestBody"] = {
                {"required", true},
                {"content",  {{"application/json", {{"schema", *route.request_body}}}}}
            };
        }

        // Responses
        if (!route.response_schema.empty()) {
            op["responses"]["200"] = {
                {"description", "Success"},
                {"content",     {{"application/json", {{"schema", route.response_schema}}}}}
            };
        } else {
            op["responses"]["204"] = {{"description", "No Content"}};
        }

        doc["paths"][oa_path][method] = op;
    }

    return doc;
}

// ─── Swagger UI HTML (CDN) ───────────────────────────────────────────────────
// Serves a self-contained Swagger UI page from unpkg CDN.
// For air-gapped deployments: embed swagger-ui-dist assets instead.

inline std::string swagger_ui_html(const std::string& spec_url = "/openapi.json") {
    return R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <title>Osodio — API Docs</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
  <style>body { margin: 0; }</style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>
    SwaggerUIBundle({
      url: ")" + spec_url + R"(",
      dom_id: '#swagger-ui',
      deepLinking: true,
      presets: [SwaggerUIBundle.presets.apis, SwaggerUIBundle.SwaggerUIStandalonePreset],
      layout: "StandaloneLayout"
    });
  </script>
</body>
</html>)";
}

} // namespace osodio
