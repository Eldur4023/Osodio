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

// --- FixedString for C++20 NTTP (used for PathParam<T, "id">) ---
template<size_t N>
struct fixed_string {
    char buf[N];
    constexpr fixed_string(const char (&s)[N]) {
        for (size_t i = 0; i < N; ++i) buf[i] = s[i];
    }
    operator std::string_view() const { return {buf, N - 1}; }
};

// --- Special parameter types ---
template<typename T, fixed_string Name>
struct PathParam {
    T value;
    operator T() const { return value; }
};

template<typename T>
struct Body {
    T value;
    Body(T v) : value(std::move(v)) {}
    const T* operator->() const { return &value; }
    const T& operator*() const { return value; }
};

template<typename T, fixed_string Name>
struct Query {
    T value;
    operator T() const { return value; }
};

// --- Extraction logic ---
template<typename T>
struct extractor {
    static T extract(const Request& req, Response& res) {
        if constexpr (std::is_same_v<T, Request&>) return const_cast<Request&>(req);
        else if constexpr (std::is_same_v<T, const Request&>) return req;
        else if constexpr (std::is_same_v<T, Response&>) return res;
        else return T{};
    }
};

template<typename T, fixed_string Name>
struct extractor<PathParam<T, Name>> {
    static PathParam<T, Name> extract(const Request& req, Response&) {
        std::string_view name = Name;
        auto it = req.params.find(std::string(name));
        if (it == req.params.end()) return {T{}};
        if constexpr (std::is_same_v<T, int>) return {std::stoi(it->second)};
        else if constexpr (std::is_same_v<T, std::string>) return {it->second};
        else return {T{}};
    }
};

template<typename T, typename = void>
struct has_validate : std::false_type {};
template<typename T>
struct has_validate<T, std::void_t<decltype(validate(std::declval<T&>()))>> : std::true_type {};

template<typename T>
struct extractor<Body<T>> {
    static Body<T> extract(const Request& req, Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            T val = j.template get<T>();
            if constexpr (has_validate<T>::value) {
                try {
                    validate(val);
                } catch (const ValidationError& e) {
                    res.status(400).json({{"error", "Validation Failed"}, {"messages", e.messages}});
                    return Body<T>(std::move(val));
                }
            }
            return Body<T>(std::move(val));
        } catch (...) {
            res.status(400).json({{"error", "Invalid JSON or schema"}});
            return Body<T>(T{});
        }
    }
};

// --- Handler Traits ---
template<typename F>
struct HandlerTraits : HandlerTraits<decltype(&F::operator())> {};

template<typename T>
struct is_task : std::false_type {};
template<typename T>
struct is_task<Task<T>> : std::true_type {};

template<typename R, typename ClassType, typename... Args>
struct HandlerTraits<R (ClassType::*)(Args ...) const> {
    using ReturnType = R;
    using ArgumentTuple = std::tuple<Args...>;

    template<typename F>
    static void call(F&& f, const Request& req, Response& res) {
        std::tuple<Args...> args{extractor<Args>::extract(req, res)...};
        if (res.status_code() >= 400) return;

        if constexpr (is_task<R>::value) {
            res.mark_async();
            auto task = std::apply(f, std::move(args));
            task.resume(); // Start coroutine

            if (task.done()) {
                res.unmark_async();
                if constexpr (!std::is_same_v<typename R::promise_type::value_type, void>) {
                    res.json(task.get_result());
                }
            } else {
                auto handle = task.handle;
                if constexpr (!std::is_same_v<typename R::promise_type::value_type, void>) {
                    handle.promise().on_complete = [res](auto val) mutable {
                        res.json(val);
                        res.complete_async();
                    };
                } else {
                    handle.promise().on_complete = [res]() mutable {
                        res.complete_async();
                    };
                }
                // Detach to keep alive (demo simplicity)
                new Task<typename R::promise_type::value_type>(std::move(task));
            }
        } else if constexpr (std::is_same_v<R, void>) {
            std::apply(f, std::move(args));
        } else {
            res.json(std::apply(f, std::move(args)));
        }
    }
};

} // namespace osodio
