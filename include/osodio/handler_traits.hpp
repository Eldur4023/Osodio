#pragma once
#include <string>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <optional>
#include "task.hpp"
#include "request.hpp"
#include "response.hpp"
#include "validation.hpp"

namespace osodio {

// ─── FixedString: C++20 NTTP string (enables PathParam<T, "id">) ─────────────

template<size_t N>
struct fixed_string {
    char buf[N];
    constexpr fixed_string(const char (&s)[N]) {
        for (size_t i = 0; i < N; ++i) buf[i] = s[i];
    }
    operator std::string_view() const { return {buf, N - 1}; }
};

// ─── Special parameter types ─────────────────────────────────────────────────

template<typename T, fixed_string Name>
struct PathParam {
    T value;
    operator T() const { return value; }
};

template<typename T>
struct Body {
    T value;
    Body() = default;
    explicit Body(T v) : value(std::move(v)) {}
    const T* operator->() const { return &value; }
    const T& operator*()  const { return value; }
};

template<typename T, fixed_string Name>
struct Query {
    T value;
    operator T() const { return value; }
};

// ─── has_validate ─────────────────────────────────────────────────────────────

template<typename T, typename = void>
struct has_validate : std::false_type {};

template<typename T>
struct has_validate<T,
    std::void_t<decltype(validate(std::declval<const T&>()))>
> : std::true_type {};

// ─── Argument extractors ─────────────────────────────────────────────────────

template<typename T>
struct extractor {
    static T extract(const Request& req, Response& res) {
        if constexpr (std::is_same_v<T, Request&>)
            return const_cast<Request&>(req);
        else if constexpr (std::is_same_v<T, const Request&>)
            return req;
        else if constexpr (std::is_same_v<T, Response&>)
            return res;
        else
            return T{};
    }
};

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

template<typename T, fixed_string Name>
struct extractor<Query<T, Name>> {
    static Query<T, Name> extract(const Request& req, Response&) {
        std::string_view name = Name;
        auto it = req.query.find(std::string(name));
        if (it == req.query.end()) return {T{}};
        if constexpr (std::is_same_v<T, int>)              return {std::stoi(it->second)};
        else if constexpr (std::is_same_v<T, std::string>) return {it->second};
        else return {T{}};
    }
};

template<typename T>
struct extractor<Body<T>> {
    static Body<T> extract(const Request& req, Response& res) {
        try {
            auto j   = nlohmann::json::parse(req.body);
            T    val = j.template get<T>();

            if constexpr (has_validate<T>::value) {
                try {
                    validate(val);
                } catch (const ValidationError& e) {
                    res.status(422).json({
                        {"error",    "Validation Failed"},
                        {"messages", e.messages}
                    });
                }
            }
            return Body<T>(std::move(val));
        } catch (const ValidationError&) {
            throw;
        } catch (...) {
            res.status(400).json({{"error", "Invalid JSON or schema mismatch"}});
            return Body<T>{};
        }
    }
};

// ─── is_task<T> ───────────────────────────────────────────────────────────────

template<typename T>
struct is_task : std::false_type {};
template<typename T>
struct is_task<Task<T>> : std::true_type {};

// ─── HandlerTraits ────────────────────────────────────────────────────────────
// Wraps any callable into a Task<void> coroutine so the entire dispatch chain
// is uniformly async.  Three cases:
//
//   - void return       → run synchronously, no serialisation
//   - Task<T> return    → co_await the inner task, then res.json(result)
//   - anything else     → res.json(return value)  (sync auto-serialise)

template<typename F>
struct HandlerTraits : HandlerTraits<decltype(&F::operator())> {};

// const lambda / functor
template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...) const> {

    template<typename F>
    static Task<void> call(F&& f, const Request& req, Response& res) {
        // Build arguments; short-circuit if an extractor already set an error.
        auto args = std::tuple<Args...>{extractor<Args>::extract(req, res)...};
        if (res.status_code() >= 400) co_return;

        if constexpr (is_task<R>::value) {
            // ── Async handler ────────────────────────────────────────────────
            using ValueType = typename R::promise_type::value_type;
            auto inner = std::apply(std::forward<F>(f), std::move(args));

            if constexpr (!std::is_void_v<ValueType>) {
                auto result = co_await inner;
                if (res.status_code() < 400) res.json(result);
            } else {
                co_await inner;
            }

        } else if constexpr (std::is_same_v<R, void>) {
            // ── Sync void handler ────────────────────────────────────────────
            std::apply(std::forward<F>(f), std::move(args));

        } else {
            // ── Sync returning handler → auto-serialise ──────────────────────
            auto result = std::apply(std::forward<F>(f), std::move(args));
            if (res.status_code() < 400) res.json(result);
        }
    }
};

// mutable lambda / non-const functor
template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...)> {
    template<typename F>
    static Task<void> call(F&& f, const Request& req, Response& res) {
        return HandlerTraits<R (C::*)(Args...) const>::template call<F>(
            std::forward<F>(f), req, res);
    }
};

} // namespace osodio
