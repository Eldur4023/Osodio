#pragma once
#include <string>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <optional>
#include <simdjson.h>
#include "task.hpp"
#include "request.hpp"
#include "response.hpp"
#include "validation.hpp"
#include "schema.hpp"   // SCHEMA macro, is_optional_v, adl_serializer<optional>
#include "errors.hpp"
#include "di.hpp"

namespace osodio {

// ─── simdjson → nlohmann conversion ─────────────────────────────────────────

namespace detail {
    inline nlohmann::json simdjson_to_nlohmann(simdjson::dom::element elem) {
        switch (elem.type()) {
            case simdjson::dom::element_type::OBJECT: {
                nlohmann::json obj = nlohmann::json::object();
                for (auto [k, v] : elem.get_object())
                    obj[std::string(k)] = simdjson_to_nlohmann(v);
                return obj;
            }
            case simdjson::dom::element_type::ARRAY: {
                nlohmann::json arr = nlohmann::json::array();
                for (auto v : elem.get_array())
                    arr.push_back(simdjson_to_nlohmann(v));
                return arr;
            }
            case simdjson::dom::element_type::STRING:
                return std::string(elem.get_string().value());
            case simdjson::dom::element_type::INT64:
                return elem.get_int64().value();
            case simdjson::dom::element_type::UINT64:
                return elem.get_uint64().value();
            case simdjson::dom::element_type::DOUBLE:
                return elem.get_double().value();
            case simdjson::dom::element_type::BOOL:
                return elem.get_bool().value();
            case simdjson::dom::element_type::NULL_VALUE:
                return nullptr;
        }
        return nullptr;
    }
} // namespace detail

// ─── FixedString ─────────────────────────────────────────────────────────────

template<size_t N>
struct fixed_string {
    char buf[N];
    constexpr fixed_string(const char (&s)[N]) {
        for (size_t i = 0; i < N; ++i) buf[i] = s[i];
    }
    operator std::string_view() const { return {buf, N - 1}; }
};

// ─── Parameter wrapper types ─────────────────────────────────────────────────

template<typename T, fixed_string Name>
struct PathParam {
    T value;
    operator T() const { return value; }
};

// Body<T> — explicit wrapper; kept for backward-compat and when you need the
// optional validity check (if (!body) { ... }).
template<typename T>
struct Body {
    T value;
    bool valid = false;
    Body() = default;
    explicit Body(T v) : value(std::move(v)), valid(true) {}
    const T* operator->() const { return &value; }
    const T& operator*()  const { return value; }
    explicit operator bool() const { return valid; }
};

// Query<T, "name">           — optional param; T{} (or "") if absent
// Query<T, "name", "default"> — param with explicit default value
template<typename T, fixed_string Name, fixed_string Default = "">
struct Query {
    T value;
    bool present = false;
    operator T() const { return value; }
    explicit operator bool() const { return present; }
};

// ─── has_validate ─────────────────────────────────────────────────────────────
// Detects structs that define a validate() method returning vector<string>.
// Replaces the old OSODIO_VALIDATE macro — just write validate() directly.

template<typename T, typename = void>
struct has_validate : std::false_type {};
template<typename T>
struct has_validate<T,
    std::enable_if_t<
        std::is_same_v<
            decltype(std::declval<const T&>().validate()),
            std::vector<std::string>
        >
    >
> : std::true_type {};

// ─── has_to_json ──────────────────────────────────────────────────────────────

template<typename T, typename = void>
struct has_to_json : std::false_type {};
template<typename T>
struct has_to_json<T,
    std::void_t<decltype(nlohmann::to_json(
        std::declval<nlohmann::json&>(), std::declval<const T&>()))>
> : std::true_type {};

// ─── is_known_param_type ──────────────────────────────────────────────────────
// Marks types that already have an explicit extractor specialisation.
// Prevents the auto-body extractor from matching them.

template<typename T> struct is_known_param_type : std::false_type {};
template<> struct is_known_param_type<Request>         : std::true_type {};
template<> struct is_known_param_type<const Request>   : std::true_type {};
template<> struct is_known_param_type<Response>        : std::true_type {};
template<> struct is_known_param_type<nlohmann::json>  : std::true_type {};
template<typename T, fixed_string N>
struct is_known_param_type<PathParam<T, N>>            : std::true_type {};
template<typename T, fixed_string N, fixed_string D>
struct is_known_param_type<Query<T, N, D>>             : std::true_type {};
template<typename T>
struct is_known_param_type<Body<T>>                    : std::true_type {};
template<typename T>
struct is_known_param_type<Inject<T>>                  : std::true_type {};

// ─── Body extraction — shared impl ───────────────────────────────────────────
// Used by both extractor<T> (auto) and extractor<Body<T>> (explicit).
// On error: sets res status and returns default-constructed T.
//
// Pipeline:
//   1. Parse JSON (simdjson — fast malformed-input rejection)
//   2. Convert to nlohmann and bind via from_json
//      - std::optional<T> fields: absent/null → nullopt (handled by SCHEMA macro)
//      - Required fields: absent → nlohmann throws → 422
//   3. Call validate() if defined — business-rule errors

namespace detail {

// Clean up nlohmann exception messages into human-readable field errors.
// "[json.exception.out_of_range.403] key 'name' not found" → "name: required"
inline std::string format_json_error(const std::string& msg) {
    if (auto k = msg.find("key '"); k != std::string::npos) {
        auto start = k + 5;
        auto end   = msg.find('\'', start);
        if (end != std::string::npos) {
            auto key = msg.substr(start, end - start);
            if (msg.find("not found") != std::string::npos)
                return key + ": required";
            if (msg.find("null")      != std::string::npos)
                return key + ": must not be null";
        }
    }
    // Strip "[json.exception.xxx.NNN] " prefix for other errors
    if (auto b = msg.find("] "); b != std::string::npos)
        return msg.substr(b + 2);
    return msg;
}

template<typename T>
T extract_body(const Request& req, Response& res) {
    // 1. Fast JSON parse + object-type check via simdjson
    thread_local simdjson::dom::parser sjparser;
    simdjson::dom::element doc;
    auto perr = sjparser.parse(req.body).get(doc);
    if (perr) {
        res.status(400).json({
            {"error",  "Invalid JSON"},
            {"detail", std::string(simdjson::error_message(perr))}
        });
        return T{};
    }
    if (doc.type() != simdjson::dom::element_type::OBJECT) {
        res.status(400).json({{"error", "Request body must be a JSON object"}});
        return T{};
    }

    // 2. Convert to nlohmann and bind.
    //    SCHEMA's from_json handles std::optional<T> fields automatically:
    //      - absent or null → std::nullopt
    //    Required fields missing → nlohmann throws out_of_range → 422.
    nlohmann::json j = simdjson_to_nlohmann(doc);
    T val;
    try {
        val = j.get<T>();
    } catch (const nlohmann::json::exception& e) {
        res.status(422).json({{"error",    "Validation Failed"},
                               {"messages", nlohmann::json::array(
                                   {format_json_error(e.what())})}});
        return T{};
    } catch (...) {
        res.status(422).json({{"error", "Schema binding failed"}});
        return T{};
    }

    // 3. Business-rule validation — define validate() in your struct.
    //    Return std::vector<std::string> with one error per entry, or {}.
    if constexpr (has_validate<T>::value) {
        auto errs = val.validate();
        if (!errs.empty()) {
            res.status(422).json({{"error", "Validation Failed"}, {"messages", errs}});
            return T{};
        }
    }

    return val;
}
} // namespace detail

// ─── Extractors ───────────────────────────────────────────────────────────────

// Primary: fires a clear compile error for unsupported types.
template<typename T, typename = void>
struct extractor {
    static T extract(const Request&, Response&) {
        static_assert(sizeof(T) == 0,
            "Unsupported handler parameter type. "
            "Supported: PathParam<T,\"name\">, Query<T,\"name\">, "
            "Inject<T>, Request&, Response&, "
            "Body<T> (explicit), or any OSODIO_SCHEMA struct (implicit body).");
    }
};

// Request& / Response& — pass through
template<>
struct extractor<Request&> {
    static Request& extract(const Request& req, Response&) {
        return const_cast<Request&>(req);
    }
};
template<>
struct extractor<const Request&> {
    static const Request& extract(const Request& req, Response&) { return req; }
};
template<>
struct extractor<Response&> {
    static Response& extract(const Request&, Response& res) { return res; }
};

// PathParam<T, Name>
template<typename T, fixed_string Name>
struct extractor<PathParam<T, Name>> {
    static PathParam<T, Name> extract(const Request& req, Response&) {
        std::string_view name = Name;
        auto it = req.params.find(std::string(name));
        if (it == req.params.end()) return {T{}};
        if constexpr (std::is_same_v<T, int>)              return {std::stoi(it->second)};
        else if constexpr (std::is_same_v<T, long>)        return {std::stol(it->second)};
        else if constexpr (std::is_same_v<T, float>)       return {std::stof(it->second)};
        else if constexpr (std::is_same_v<T, double>)      return {std::stod(it->second)};
        else if constexpr (std::is_same_v<T, std::string>) return {it->second};
        else return {T{}};
    }
};

// Query<T, Name, Default>
// If param is present → parse it; if absent → use Default string (converted to T).
template<typename T, fixed_string Name, fixed_string Default>
struct extractor<Query<T, Name, Default>> {
    static Query<T, Name, Default> extract(const Request& req, Response&) {
        std::string_view name = Name;
        auto it = req.query.find(std::string(name));

        // Helper: parse a string value to T
        auto parse = [](const std::string& s) -> T {
            if constexpr (std::is_same_v<T, int>)              return s.empty() ? T{} : std::stoi(s);
            else if constexpr (std::is_same_v<T, long>)        return s.empty() ? T{} : std::stol(s);
            else if constexpr (std::is_same_v<T, float>)       return s.empty() ? T{} : std::stof(s);
            else if constexpr (std::is_same_v<T, double>)      return s.empty() ? T{} : std::stod(s);
            else if constexpr (std::is_same_v<T, bool>)        return s == "true" || s == "1";
            else if constexpr (std::is_same_v<T, std::string>) return s;
            else return T{};
        };

        if (it != req.query.end())
            return {parse(it->second), true};

        // Absent: use Default if non-empty, otherwise T{}
        std::string_view def = Default;
        if (!def.empty())
            return {parse(std::string(def)), false};
        return {T{}, false};
    }
};

// Body<T> — explicit wrapper (backward-compat; also provides operator bool)
template<typename T>
struct extractor<Body<T>> {
    static Body<T> extract(const Request& req, Response& res) {
        T val = detail::extract_body<T>(req, res);
        if (res.status_code() >= 400) return Body<T>{};
        return Body<T>(std::move(val));
    }
};

// Inject<T>
template<typename T>
struct extractor<Inject<T>> {
    static Inject<T> extract(const Request& req, Response& res) {
        if (!req.container) {
            res.status(500).json({{"error", "No service container configured"}});
            return {};
        }
        auto ptr = req.container->template resolve<T>();
        if (!ptr) {
            res.status(500).json({{"error", "Service not registered"},
                                  {"type",  typeid(T).name()}});
            return {};
        }
        return Inject<T>{std::move(ptr)};
    }
};

// Auto-body for OSODIO_SCHEMA types — the ergonomic path.
// [](User body) { ... }  works without Body<> wrapper.
// Matches any class with OSODIO_SCHEMA that isn't already a known param type.
template<typename T>
struct extractor<T, std::enable_if_t<
    std::is_class_v<T>                          &&
    !is_known_param_type<T>::value              &&
    has_to_json<T>::value
>> {
    static T extract(const Request& req, Response& res) {
        return detail::extract_body<T>(req, res);
    }
};

// ─── is_task<T> ───────────────────────────────────────────────────────────────

template<typename T>
struct is_task : std::false_type {};
template<typename T>
struct is_task<Task<T>> : std::true_type {};

// ─── HandlerTraits ────────────────────────────────────────────────────────────
// Wraps any callable into Task<void>.  Supports:
//   - [](User body) { return User{...}; }          sync, auto-body, auto-json
//   - [](PathParam<int,"id"> id) -> User { ... }   sync, path param
//   - [](Inject<DB> db) -> Task<json> { co_return ... }  async
//   - throw osodio::not_found()                    caught here → JSON 404

template<typename F>
struct HandlerTraits : HandlerTraits<decltype(&F::operator())> {};

template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...) const> {
    template<typename F>
    static Task<void> call(F&& f, const Request& req, Response& res) {
        auto args = std::tuple<Args...>{extractor<Args>::extract(req, res)...};
        if (res.status_code() >= 400) co_return;

        try {
            if constexpr (is_task<R>::value) {
                using V = typename R::promise_type::value_type;
                auto inner = std::apply(std::forward<F>(f), std::move(args));
                if constexpr (!std::is_void_v<V>) {
                    auto result = co_await inner;
                    if (res.status_code() < 400) res.json(result);
                } else {
                    co_await inner;
                }
            } else if constexpr (std::is_same_v<R, void>) {
                std::apply(std::forward<F>(f), std::move(args));
            } else {
                auto result = std::apply(std::forward<F>(f), std::move(args));
                if (res.status_code() < 400) res.json(result);
            }
        } catch (const HttpError& e) {
            res.status(e.status).json(e.body);
        }
    }
};

template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...)> {
    template<typename F>
    static Task<void> call(F&& f, const Request& req, Response& res) {
        return HandlerTraits<R (C::*)(Args...) const>::template call<F>(
            std::forward<F>(f), req, res);
    }
};

} // namespace osodio
