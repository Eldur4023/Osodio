#pragma once
#include <functional>
#include <unordered_map>
#include <cstdint>

namespace osodio::core {

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    using Callback = std::function<void(uint32_t events)>;

    void add   (int fd, uint32_t events, Callback cb);
    void modify(int fd, uint32_t events);
    void remove(int fd);

    // Blocks until stop() is called
    void run();
    void stop();

private:
    int  epoll_fd_  = -1;
    int  wakeup_fd_ = -1;   // eventfd used to interrupt epoll_wait
    bool running_   = false;

    std::unordered_map<int, Callback> callbacks_;
};

} // namespace osodio::core
