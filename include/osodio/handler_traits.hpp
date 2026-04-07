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

// ─── has_validate: detects free function validate(T&) via ADL ────────────────

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
        if constexpr (std::is_same_v<T, int>)         return {std::stoi(it->second)};
        else if constexpr (std::is_same_v<T, long>)   return {std::stol(it->second)};
        else if constexpr (std::is_same_v<T, float>)  return {std::stof(it->second)};
        else if constexpr (std::is_same_v<T, double>)  return {std::stod(it->second)};
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
        if constexpr (std::is_same_v<T, int>)         return {std::stoi(it->second)};
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
            throw; // already handled above
        } catch (...) {
            res.status(400).json({{"error", "Invalid JSON or schema mismatch"}});
            return Body<T>{};
        }
    }
};

// ─── is_task<T> ──────────────────────────────────────────────────────────────

template<typename T>
struct is_task : std::false_type {};
template<typename T>
struct is_task<Task<T>> : std::true_type {};

// ─── HandlerTraits ───────────────────────────────────────────────────────────
// Deduces the signature of a callable and dispatches it with auto-extracted
// arguments, handling three return types:
//   - void         → call and do nothing extra
//   - Task<T>      → async path (coroutine)
//   - anything else → serialize to JSON response

template<typename F>
struct HandlerTraits : HandlerTraits<decltype(&F::operator())> {};

// const lambda / functor
template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...) const> {

    template<typename F>
    static void call(F&& f, const Request& req, Response& res) {
        // Build arguments via extractors; short-circuit if an extractor
        // already set an error response (e.g. bad JSON body).
        auto args = std::tuple<Args...>{extractor<Args>::extract(req, res)...};
        if (res.status_code() >= 400) return;

        if constexpr (is_task<R>::value) {
            // ── Async path ────────────────────────────────────────────────
            using ValueType = typename R::promise_type::value_type;

            res.mark_async();
            auto task = std::apply(std::forward<F>(f), std::move(args));

            // Detach the handle so ~Task doesn't destroy the frame prematurely.
            // The frame is owned by the promise and destroyed by FinalAwaitable
            // via the event loop once the coroutine completes.
            auto h = task.detach();

            // Wire up the event loop for deferred frame destruction
            h.promise().loop = const_cast<core::EventLoop*>(req.loop);

            // Set the completion callback BEFORE resuming.
            // For the sync-completion path this is still safe: on_complete_cb
            // in Response::State hasn't been set yet so complete_async() is a
            // no-op, and http_connection will detect is_async()==false.
            if constexpr (!std::is_void_v<ValueType>) {
                h.promise().on_complete = [res](ValueType val) mutable {
                    res.json(val);
                    res.complete_async();
                };
            } else {
                h.promise().on_complete = [res]() mutable {
                    res.complete_async();
                };
            }

            h.resume(); // Start the coroutine

            if (h.done()) {
                // Completed synchronously: body already set by on_complete,
                // FinalAwaitable has scheduled h.destroy() via the loop.
                res.unmark_async();
            }
            // If not done: FinalAwaitable will call loop->post(h.destroy)
            // once the coroutine reaches co_return.

        } else if constexpr (std::is_same_v<R, void>) {
            // ── Sync void handler ─────────────────────────────────────────
            std::apply(std::forward<F>(f), std::move(args));

        } else {
            // ── Sync returning handler → auto-serialize to JSON ───────────
            res.json(std::apply(std::forward<F>(f), std::move(args)));
        }
    }
};

// mutable lambda / non-const functor
template<typename R, typename C, typename... Args>
struct HandlerTraits<R (C::*)(Args...)> {
    template<typename F>
    static void call(F&& f, const Request& req, Response& res) {
        HandlerTraits<R (C::*)(Args...) const>::template call<F>(
            std::forward<F>(f), req, res);
    }
};

} // namespace osodio
