#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include <chrono>
#include <thread>
#include <functional>
#include <osodio/core/event_loop.hpp>

namespace osodio {

// ─── Task<T> ─────────────────────────────────────────────────────────────────
//
// Coroutine return type. Supports:
//   - co_return value
//   - co_await sleep(ms, loop)
//   - co_await Task<U>          (chained tasks via symmetric transfer)
//
// Lifetime: the coroutine frame self-destructs via the event loop after
// completing, as long as loop is set on the promise before resuming.
// Use detach() to transfer ownership from the Task wrapper to the promise.

template<typename T>
struct Task {
    struct promise_type {
        using value_type = T;

        T               result;
        std::exception_ptr exception;
        std::function<void(T)> on_complete;   // called when the task finishes
        std::coroutine_handle<> continuation; // outer coroutine waiting on us
        core::EventLoop* loop = nullptr;      // for deferred self-destruction

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        // After the coroutine finishes:
        //   - if there is a continuation (co_await chain), resume it
        //   - otherwise schedule self-destruction via the event loop
        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto c = h.promise().continuation) return c;  // symmetric transfer

                // No continuation — schedule destruction on the event loop
                if (h.promise().loop) {
                    auto raw = std::coroutine_handle<>(h);
                    h.promise().loop->post([raw]() mutable { raw.destroy(); });
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_value(T value) {
            result = std::move(value);
            if (on_complete) on_complete(result);
        }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task(const Task&) = delete;
    ~Task() { if (handle) handle.destroy(); }

    // Transfer ownership to the promise (stops ~Task from destroying the frame)
    std::coroutine_handle<promise_type> detach() {
        return std::exchange(handle, nullptr);
    }

    bool done() const { return handle && handle.done(); }

    T get_result() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }

    // ── Awaiter interface (for co_await Task<T> inside another coroutine) ────
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> outer) noexcept {
        // Store the outer coroutine as our continuation; FinalAwaitable resumes it
        handle.promise().continuation = outer;
        handle.resume(); // start the inner task
    }

    T await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }
};

// ─── Task<void> ──────────────────────────────────────────────────────────────

template<>
struct Task<void> {
    struct promise_type {
        using value_type = void;

        std::exception_ptr exception;
        std::function<void()> on_complete;
        std::coroutine_handle<> continuation;
        core::EventLoop* loop = nullptr;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto c = h.promise().continuation) return c;

                if (h.promise().loop) {
                    auto raw = std::coroutine_handle<>(h);
                    h.promise().loop->post([raw]() mutable { raw.destroy(); });
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_void() {
            if (on_complete) on_complete();
        }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task(const Task&) = delete;
    ~Task() { if (handle) handle.destroy(); }

    std::coroutine_handle<promise_type> detach() {
        return std::exchange(handle, nullptr);
    }

    bool done() const { return handle && handle.done(); }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> outer) noexcept {
        handle.promise().continuation = outer;
        handle.resume();
    }
    void await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
    }
};

// ─── SleepAwaitable ──────────────────────────────────────────────────────────
// Non-blocking sleep: suspends the coroutine and posts resume to the event
// loop after `ms` milliseconds (via a detached thread — good enough for now;
// a timerfd-based implementation would be more scalable).

struct SleepAwaitable {
    int ms;
    core::EventLoop* loop;

    bool await_ready() const noexcept { return ms <= 0; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        if (!loop) { h.resume(); return; }
        std::thread([ms = ms, loop = loop, h]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            loop->post([h]() mutable { if (!h.done()) h.resume(); });
        }).detach();
    }

    void await_resume() const noexcept {}
};

inline SleepAwaitable sleep(int ms, core::EventLoop* loop) {
    return {ms, loop};
}

} // namespace osodio
