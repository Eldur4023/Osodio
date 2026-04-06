#include "event_loop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <iostream>

namespace osodio::core {

EventLoop::EventLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));

    // eventfd used to wake up epoll_wait from stop()
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw std::runtime_error(std::string("eventfd: ") + strerror(errno));

    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.fd  = wakeup_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
}

EventLoop::~EventLoop() {
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    if (epoll_fd_  >= 0) ::close(epoll_fd_);
}

void EventLoop::add(int fd, uint32_t events, Callback cb) {
    callbacks_[fd] = std::move(cb);

    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        callbacks_.erase(fd);
        std::cerr << "epoll_ctl ADD fd=" << fd << ": " << strerror(errno) << '\n';
    }
}

void EventLoop::modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EventLoop::remove(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

void EventLoop::run() {
    running_ = true;
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    while (running_) {
        int n = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait: " << strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // Wakeup fd → check if we should stop
            if (fd == wakeup_fd_) {
                uint64_t val;
                (void)read(wakeup_fd_, &val, sizeof(val));
                continue;
            }

            auto it = callbacks_.find(fd);
            if (it == callbacks_.end()) continue;

            // Make a local copy so the callback isn't destroyed if it
            // calls remove() on itself (e.g. connection close).
            Callback cb = it->second;
            cb(events[i].events);
        }
    }
}

void EventLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    (void)write(wakeup_fd_, &val, sizeof(val));
}

} // namespace osodio::core
