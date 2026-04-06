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
