#include "http_connection.hpp"

#include "../../include/osodio/request.hpp"
#include "../../include/osodio/response.hpp"

#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace osodio::http {

// ─── Query string helpers ────────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned int hex = 0;
            std::istringstream iss(s.substr(i + 1, 2));
            iss >> std::hex >> hex;
            out += static_cast<char>(hex);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static void parse_query(const std::string& qs,
                         std::unordered_map<std::string, std::string>& out) {
    if (qs.empty()) return;
    std::istringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos)
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        else if (!pair.empty())
            out[url_decode(pair)] = "";
    }
}

// ─── HttpConnection ──────────────────────────────────────────────────────────

HttpConnection::HttpConnection(int fd, core::EventLoop& loop,
                               osodio::DispatchFn dispatch)
    : fd_(fd)
    , loop_(loop)
    , dispatch_(std::move(dispatch))
    , parser_([this](ParsedRequest req) { this->dispatch(std::move(req)); })
{}

HttpConnection::~HttpConnection() {
    if (!closed_) close();
}

void HttpConnection::on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close(); return; }
    if (events & EPOLLIN) do_read();
}

void HttpConnection::do_read() {
    char buf[16384];
    while (!closed_) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n == 0)  { close(); return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            close(); return;
        }
        if (!parser_.feed(buf, static_cast<size_t>(n))) {
            send_error(400, "Bad Request");
            close();
            return;
        }
    }
}

void HttpConnection::dispatch(ParsedRequest req) {
    osodio::Request request;
    request.method  = req.method;
    request.path    = req.path;
    request.version = req.version;
    request.headers = std::move(req.headers);
    request.body    = std::move(req.body);
    parse_query(req.query, request.query);

    osodio::Response response;

    try {
        dispatch_(request, response);   // ← middlewares + router
    } catch (const std::exception& e) {
        response = osodio::Response{};
        response.status(500).json({{"error", e.what()}});
    } catch (...) {
        response = osodio::Response{};
        response.status(500).json({{"error", "Internal Server Error"}});
    }

    // Keep-alive
    bool keep_alive = (request.version == "HTTP/1.1");
    auto conn_hdr = request.header("connection");
    if (conn_hdr) {
        std::string val = *conn_hdr;
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        keep_alive = (val != "close");
    }
    response.header("Connection", keep_alive ? "keep-alive" : "close");

    send_response(response.build());
    if (!keep_alive) close();
}

void HttpConnection::send_response(const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(fd_, data.data() + total, data.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(); return;
        }
        total += static_cast<size_t>(n);
    }
}

void HttpConnection::send_error(int code, const char* msg) {
    osodio::Response r;
    r.status(code).json({{"error", msg}});
    r.header("Connection", "close");
    send_response(r.build());
}

void HttpConnection::close() {
    if (closed_) return;
    closed_ = true;
    loop_.remove(fd_);
    ::close(fd_);
}

} // namespace osodio::http
