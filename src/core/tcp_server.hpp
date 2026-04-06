#pragma once
#include <string>
#include <cstdint>
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"

namespace osodio::core {

class TcpServer {
public:
    TcpServer(const std::string& host, uint16_t port,
              EventLoop& loop, osodio::DispatchFn dispatch);
    ~TcpServer();

private:
    int               listen_fd_ = -1;
    EventLoop&        loop_;
    osodio::DispatchFn dispatch_;

    void on_accept();
};

} // namespace osodio::core
