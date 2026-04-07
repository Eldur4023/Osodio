#pragma once
#include <string>
#include <memory>
#include <cstdint>
#include "http_parser.hpp"
#include <osodio/core/event_loop.hpp>
#include "../../include/osodio/types.hpp"

namespace osodio::http {

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(int fd, core::EventLoop& loop, osodio::DispatchFn dispatch);
    ~HttpConnection();

    void on_event(uint32_t events);

private:
    int                fd_;
    core::EventLoop&   loop_;
    osodio::DispatchFn dispatch_;
    HttpParser         parser_;
    bool               closed_ = false;

    void do_read();
    void dispatch(ParsedRequest req);
    void finish_dispatch(osodio::Request& request, osodio::Response& response);
    void send_response(const std::string& data);
    void send_error(int code, const char* msg);
    void close();
};

} // namespace osodio::http
