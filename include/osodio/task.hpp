#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include <chrono>
#include <thread>
#include <functional>
#include <osodio/core/event_loop.hpp>

namespace osodio {

template<typename T>
struct Task {
    struct promise_type {
        using value_type = T;
        T result;
        std::exception_ptr exception;
        std::function<void(T)> on_complete;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() { exception = std::current_exception(); }
        void return_value(T value) { 
            result = std::move(value); 
            if (on_complete) on_complete(result);
        }
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    ~Task() { if (handle) handle.destroy(); }

    bool done() const { return handle.done(); }
    void resume() { if (handle && !handle.done()) handle.resume(); }
    
    T get_result() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }

    // Awaiter interface
    bool await_ready() const noexcept { return handle.done(); }
    void await_suspend(std::coroutine_handle<> awaiting_handle) noexcept {}
    T await_resume() { return get_result(); }
};

template<>
struct Task<void> {
    struct promise_type {
        using value_type = void;
        std::exception_ptr exception;
        std::function<void()> on_complete;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() { exception = std::current_exception(); }
        void return_void() {
            if (on_complete) on_complete();
        }
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    ~Task() { if (handle) handle.destroy(); }

    bool done() const { return handle.done(); }
    void resume() { if (handle && !handle.done()) handle.resume(); }

    void get_result() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
    }

    bool await_ready() const noexcept { return handle.done(); }
    void await_suspend(std::coroutine_handle<> awaiting_handle) noexcept {}
    void await_resume() { get_result(); }
};

struct SleepAwaitable {
    int ms;
    core::EventLoop* loop;

    bool await_ready() const noexcept { return ms <= 0; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        if (!loop) { h.resume(); return; }
        std::thread([this, h]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            loop->post([h]() { if (h && !h.done()) h.resume(); });
        }).detach();
    }
    void await_resume() const noexcept {}
};

inline SleepAwaitable sleep(int ms, core::EventLoop* loop) {
    return {ms, loop};
}

} // namespace osodio
