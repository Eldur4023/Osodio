#include "http_connection.hpp"
#include "../../include/osodio/request.hpp"
#include "../../include/osodio/response.hpp"
#include "../../include/osodio/task.hpp"

#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace osodio::http {

// ── URL helpers ───────────────────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned int hex = 0;
            char buf[3] = {s[i+1], s[i+2], '\0'};
            hex = static_cast<unsigned int>(std::strtoul(buf, nullptr, 16));
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

// ── HttpConnection ────────────────────────────────────────────────────────────

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

void HttpConnection::dispatch(ParsedRequest req_parsed) {
    auto req_ptr = std::make_shared<osodio::Request>();
    auto res_ptr = std::make_shared<osodio::Response>();

    req_ptr->method  = req_parsed.method;
    req_ptr->path    = req_parsed.path;
    req_ptr->version = req_parsed.version;
    req_ptr->headers = std::move(req_parsed.headers);
    req_ptr->body    = std::move(req_parsed.body);
    req_ptr->loop    = &loop_;
    parse_query(req_parsed.query, req_ptr->query);

    // ── Wrapper coroutine keeps req_ptr / res_ptr alive for the full chain ──
    // req_ptr and res_ptr are captured by value (shared_ptr copies stored in
    // the coroutine frame), ensuring they outlive any co_await suspension.
    auto wrapper_task = [req_ptr, res_ptr, disp = dispatch_]() -> osodio::Task<void> {
        try {
            co_await disp(*req_ptr, *res_ptr);
        } catch (const std::exception& e) {
            res_ptr->status(500).json({{"error", e.what()}});
        } catch (...) {
            res_ptr->status(500).json({{"error", "Internal Server Error"}});
        }
    }();

    auto h = wrapper_task.detach();
    h.promise().loop = &loop_;

    auto self = shared_from_this();
    h.promise().on_complete = [self, req_ptr, res_ptr]() {
        self->finish_dispatch(*req_ptr, *res_ptr);
    };

    h.resume();
    // Whether sync or async, finish_dispatch is called via on_complete.
}

void HttpConnection::finish_dispatch(osodio::Request& request,
                                     osodio::Response& response) {
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
