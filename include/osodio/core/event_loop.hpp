#pragma once
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <mutex>

namespace osodio::core {

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    using Callback = std::function<void(uint32_t events)>;

    void add   (int fd, uint32_t events, Callback cb);
    void modify(int fd, uint32_t events);
    void remove(int fd);

    // Schedule a task to run in the next loop iteration
    void post(std::function<void()> cb);

    // Schedule a one-shot timer: fires cb after `ms` milliseconds.
    // Uses timerfd — scalable, no extra threads.
    // Returns the timerfd so it can be cancelled with cancel_timer().
    int  schedule_timer(int ms, std::function<void()> cb);

    // Cancel a previously scheduled timer (safe to call with -1 or already-fired tfd).
    void cancel_timer(int tfd);

    // Blocks until stop() is called
    void run();
    void stop();

private:
    void process_tasks();

    int  epoll_fd_  = -1;
    int  wakeup_fd_ = -1;
    bool running_   = false;

    std::unordered_map<int, Callback> callbacks_;
    std::vector<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
};

} // namespace osodio::core
