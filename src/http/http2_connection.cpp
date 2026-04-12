#include "http2_connection.hpp"
#include "../../include/osodio/request.hpp"
#include "../../include/osodio/task.hpp"
#include "../../include/osodio/metrics.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace osodio::http {

// ── URL helpers (identical to those in http_connection.cpp) ───────────────────

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char buf[3] = {s[i+1], s[i+2], '\0'};
            out += static_cast<char>(std::strtoul(buf, nullptr, 16));
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

// ── Construction / destruction ────────────────────────────────────────────────

Http2Connection::Http2Connection(int fd, SSL* ssl, core::EventLoop& loop,
                                 osodio::DispatchFn dispatch,
                                 std::shared_ptr<std::atomic<int>> conn_count)
    : fd_(fd)
    , ssl_(ssl)
    , loop_(loop)
    , dispatch_(std::move(dispatch))
    , conn_count_(std::move(conn_count))
{}

Http2Connection::~Http2Connection() {
    if (!closed_) {
        if (session_) { nghttp2_session_del(session_); session_ = nullptr; }
        if (ssl_)     { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        ::close(fd_);
    }
}

// ── init ──────────────────────────────────────────────────────────────────────

bool Http2Connection::init() {
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);

    nghttp2_session_callbacks_set_send_callback            (cbs, send_cb);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback       (cbs, on_header_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback   (cbs, on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback (cbs, on_stream_close_cb);

    int r = nghttp2_session_server_new(&session_, cbs, this);
    nghttp2_session_callbacks_del(cbs);

    if (r != 0) {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        ::close(fd_);
        if (conn_count_) conn_count_->fetch_sub(1, std::memory_order_relaxed);
        closed_ = true;
        return false;
    }

    // Server SETTINGS: max 100 concurrent streams, default window size,
    // and ENABLE_CONNECT_PROTOCOL=1 so clients can use RFC 8441 WebSocket.
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,    100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,     65535},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL,     1},
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 3);
    flush();  // send SETTINGS immediately
    return true;
}

// ── Event dispatch ────────────────────────────────────────────────────────────

void Http2Connection::on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close(); return; }
    if (events & EPOLLIN)               do_read();
    if (!closed_ && (events & EPOLLOUT)) do_write();
}

// ── Read path ─────────────────────────────────────────────────────────────────

void Http2Connection::do_read() {
    uint8_t buf[16384];
    while (!closed_) {
        ssize_t n = SSL_read(ssl_, buf, sizeof(buf));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, static_cast<int>(n));
            if (err == SSL_ERROR_WANT_READ)  return;
            if (err == SSL_ERROR_ZERO_RETURN) { close(); return; }
            close(); return;
        }
        ssize_t consumed = nghttp2_session_mem_recv(session_, buf,
                                                     static_cast<size_t>(n));
        if (consumed < 0) { close(); return; }

        // Send any frames nghttp2 wants to emit (SETTINGS ACK, WINDOW_UPDATE…)
        flush();
    }
}

// ── Write path ────────────────────────────────────────────────────────────────

void Http2Connection::flush() {
    // nghttp2_session_send() calls send_cb() repeatedly, which appends to
    // write_buf_.  Then do_write() drains it via SSL_write.
    nghttp2_session_send(session_);
    do_write();
}

void Http2Connection::do_write() {
    while (write_offset_ < write_buf_.size()) {
        ssize_t n = SSL_write(ssl_,
                              write_buf_.data()  + write_offset_,
                              static_cast<int>(write_buf_.size() - write_offset_));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, static_cast<int>(n));
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                loop_.modify(fd_, EPOLLIN | EPOLLOUT);
                return;
            }
            close(); return;
        }
        write_offset_ += static_cast<size_t>(n);
    }
    write_buf_.clear();
    write_offset_ = 0;

    // Keep listening for both reads and nghttp2 flow-control write windows
    if (!closed_) loop_.modify(fd_, EPOLLIN | EPOLLOUT);
}

// ── Stream dispatch ───────────────────────────────────────────────────────────

void Http2Connection::dispatch_stream(int32_t stream_id, Stream& s) {
    auto req_ptr = std::make_shared<osodio::Request>();
    auto res_ptr = std::make_shared<osodio::Response>();

    // Split path and query from ":path" pseudo-header
    std::string path  = s.path;
    std::string query;
    auto q = path.find('?');
    if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }

    req_ptr->method  = s.method;
    req_ptr->path    = path;
    req_ptr->version = "HTTP/2";
    req_ptr->body    = std::move(s.body);
    req_ptr->headers = std::move(s.req_headers);
    req_ptr->loop    = &loop_;
    req_ptr->_conn_fd = fd_;
    if (!s.authority.empty()) req_ptr->headers["host"] = s.authority;
    parse_query(query, req_ptr->query);

    // Remote IP from the underlying socket
    sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
        char ip[INET6_ADDRSTRLEN]{};
        if      (ss.ss_family == AF_INET)
            inet_ntop(AF_INET,  &reinterpret_cast<sockaddr_in* >(&ss)->sin_addr,  ip, sizeof(ip));
        else if (ss.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr, ip, sizeof(ip));
        req_ptr->remote_ip = ip;
    }

    // Each stream gets its own CancellationToken
    auto token = std::make_shared<osodio::CancellationToken>();
    s.cancel_token        = token;
    req_ptr->cancel_token = token;

    // Wire up the H2SSEContext so that make_sse() can push DATA frames.
    // All closures run on the event loop thread (same as nghttp2).
    auto ctx = std::make_shared<osodio::Request::H2SSEContext>();
    auto self_weak = weak_from_this();
    auto res_weak  = std::weak_ptr<osodio::Response>(res_ptr);
    ctx->begin = [self_weak, stream_id, res_weak]() {
        if (auto self = self_weak.lock())
            if (auto res = res_weak.lock())
                self->h2_begin_sse(stream_id, *res);
    };
    ctx->push = [self_weak, stream_id](std::string chunk) {
        if (auto self = self_weak.lock())
            self->h2_push_sse(stream_id, std::move(chunk));
    };
    ctx->end = [self_weak, stream_id]() {
        if (auto self = self_weak.lock())
            self->h2_end_sse(stream_id);
    };
    req_ptr->_h2_sse_ctx = std::move(ctx);

    // Wire up H2WSContext for RFC 8441 WebSocket streams.
    if (s.ws_protocol && s.method == "CONNECT") {
        // Reroute as GET so the router finds the app.ws() route.
        req_ptr->method = "GET";

        auto ws_ctx = std::make_shared<osodio::Request::H2WSContext>();
        ws_ctx->begin = [self_weak, stream_id](std::function<void(const uint8_t*, size_t)> feed_cb) {
            if (auto self = self_weak.lock())
                self->h2_begin_ws(stream_id, std::move(feed_cb));
        };
        ws_ctx->push = [self_weak, stream_id](std::string frame) {
            if (auto self = self_weak.lock())
                self->h2_push_ws(stream_id, std::move(frame));
        };
        ws_ctx->close_stream = [self_weak, stream_id]() {
            if (auto self = self_weak.lock())
                self->h2_end_ws(stream_id);
        };
        req_ptr->_h2_ws_ctx = std::move(ws_ctx);
    }

    osodio::detail::current_loop  = &loop_;
    osodio::detail::current_token = token;

    auto wrapper = [](std::shared_ptr<osodio::Request>  req,
                      std::shared_ptr<osodio::Response> res,
                      osodio::DispatchFn                disp) -> osodio::Task<void> {
        try { co_await disp(*req, *res); }
        catch (const std::exception& e) { res->status(500).json({{"error", e.what()}}); }
        catch (...)                     { res->status(500).json({{"error", "Internal Server Error"}}); }
    }(req_ptr, res_ptr, dispatch_);

    auto h = wrapper.detach();
    h.promise().loop = &loop_;

    auto self = shared_from_this();
    h.promise().on_complete = [self, stream_id, res_ptr]() {
        self->finish_stream(stream_id, *res_ptr);
    };
    h.resume();
}

void Http2Connection::finish_stream(int32_t stream_id, const osodio::Response& res) {
    if (closed_) return;

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return; // stream already closed (connection reset etc.)

    osodio::Metrics::instance().record(res.status_code());

    // SSE over HTTP/2: headers were already submitted by h2_begin_sse() and the
    // stream will be ended (END_STREAM) by h2_end_sse() when SSEWriter is
    // destroyed.  Nothing to do here except flush any pending frames.
    if (res.sse_started()) {
        flush();
        return;
    }

    // WebSocket over HTTP/2 (RFC 8441): headers + DATA frames already submitted
    // by h2_begin_ws / h2_end_ws — nothing left to do here.
    if (res.ws_started()) {
        flush();
        return;
    }

    // ── Build :status + response headers ──────────────────────────────────────
    std::string status_str = std::to_string(res.status_code());

    // nghttp2_nv borrows the strings — keep them alive in local storage.
    std::vector<std::pair<std::string,std::string>> hdr_store;
    hdr_store.reserve(res.headers_map().size() + 1);
    hdr_store.emplace_back(":status", status_str);
    for (const auto& [k, v] : res.headers_map()) {
        // HTTP/2 forbids connection-specific headers (RFC 7540 §8.1.2.2)
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == "connection" || lk == "keep-alive" ||
            lk == "transfer-encoding" || lk == "upgrade") continue;
        hdr_store.emplace_back(k, v);
    }

    std::vector<nghttp2_nv> nva;
    nva.reserve(hdr_store.size());
    for (auto& [n, v] : hdr_store) {
        nghttp2_nv nv;
        nv.name     = reinterpret_cast<uint8_t*>(n.data());
        nv.namelen  = n.size();
        nv.value    = reinterpret_cast<uint8_t*>(v.data());
        nv.valuelen = v.size();
        nv.flags    = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    // ── Resolve body ──────────────────────────────────────────────────────────
    // sendfile path: read file into the body_src buffer (TLS means no kernel bypass)
    auto& stream = it->second;
    stream.body_src = std::make_unique<BodySrc>();

    if (!res.sendfile_path().empty()) {
        std::ifstream f(res.sendfile_path(), std::ios::binary);
        if (!f) {
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE,
                                      stream_id, NGHTTP2_INTERNAL_ERROR);
            flush(); return;
        }
        stream.body_src->data.assign((std::istreambuf_iterator<char>(f)), {});
    } else {
        stream.body_src->data = res.body();
    }

    // ── Submit response ───────────────────────────────────────────────────────
    if (stream.body_src->data.empty()) {
        nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), nullptr);
    } else {
        nghttp2_data_provider prd;
        prd.source.ptr = stream.body_src.get();
        prd.read_callback = [](nghttp2_session*, int32_t,
                                uint8_t* buf, size_t len,
                                uint32_t* flags,
                                nghttp2_data_source* src,
                                void*) -> ssize_t {
            auto* s = static_cast<BodySrc*>(src->ptr);
            size_t rem = s->data.size() - s->offset;
            size_t n   = std::min(rem, len);
            std::memcpy(buf, s->data.data() + s->offset, n);
            s->offset += n;
            if (s->offset >= s->data.size())
                *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<ssize_t>(n);
        };
        nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), &prd);
    }

    flush();
}

// ── SSE over HTTP/2 ───────────────────────────────────────────────────────────
//
// h2_begin_sse — called by H2SSEContext::begin (which is invoked by make_sse()).
// Submits the HEADERS frame (status 200 + SSE headers, no END_STREAM) and
// registers a data provider whose read_callback drains the per-stream
// sse_pending queue, deferring when it is empty.
//
void Http2Connection::h2_begin_sse(int32_t stream_id, const osodio::Response& res) {
    if (closed_) return;
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    it->second.sse_mode = true;

    // Build header list — filter connection-specific headers forbidden by H2.
    std::string status_str = std::to_string(res.status_code());
    std::vector<std::pair<std::string,std::string>> hdr_store;
    hdr_store.reserve(res.headers_map().size() + 1);
    hdr_store.emplace_back(":status", status_str);
    for (const auto& [k, v] : res.headers_map()) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == "connection" || lk == "keep-alive" ||
            lk == "transfer-encoding" || lk == "upgrade") continue;
        hdr_store.emplace_back(k, v);
    }

    std::vector<nghttp2_nv> nva;
    nva.reserve(hdr_store.size());
    for (auto& [n, v] : hdr_store) {
        nghttp2_nv nv;
        nv.name     = reinterpret_cast<uint8_t*>(n.data());
        nv.namelen  = n.size();
        nv.value    = reinterpret_cast<uint8_t*>(v.data());
        nv.valuelen = v.size();
        nv.flags    = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    // Data provider: the read_callback is the only part nghttp2 uses at call
    // time; it invokes it later on nghttp2_session_send().
    nghttp2_data_provider prd{};
    prd.read_callback = [](nghttp2_session*, int32_t sid,
                            uint8_t* buf, size_t len,
                            uint32_t* flags,
                            nghttp2_data_source* /*src*/,
                            void* user_data) -> ssize_t {
        auto* self = static_cast<Http2Connection*>(user_data);
        auto it2 = self->streams_.find(sid);
        if (it2 == self->streams_.end()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        auto& s = it2->second;
        if (s.sse_pending.empty()) {
            if (s.sse_ended) {
                *flags |= NGHTTP2_DATA_FLAG_EOF;
                return 0;
            }
            s.sse_deferred = true;
            return NGHTTP2_ERR_DEFERRED;
        }
        size_t written = 0;
        while (!s.sse_pending.empty() && written < len) {
            auto& chunk = s.sse_pending.front();
            size_t n = std::min(chunk.size(), len - written);
            std::memcpy(buf + written, chunk.data(), n);
            written += n;
            if (n == chunk.size()) {
                s.sse_pending.erase(s.sse_pending.begin());
            } else {
                chunk.erase(0, n);
                break;
            }
        }
        s.sse_deferred = false;
        if (s.sse_pending.empty() && s.sse_ended)
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<ssize_t>(written);
    };

    nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), &prd);
    flush();
}

// h2_push_sse — enqueue a formatted SSE event string and resume the provider.
void Http2Connection::h2_push_sse(int32_t stream_id, std::string chunk) {
    if (closed_) return;
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.sse_mode) return;

    it->second.sse_pending.push_back(std::move(chunk));

    if (it->second.sse_deferred) {
        it->second.sse_deferred = false;
        nghttp2_session_resume_data(session_, stream_id);
    }
    flush();
}

// h2_end_sse — called when the SSEWriter is destroyed (handler exited).
// Marks the stream as ended so the data provider will set EOF on the next
// read_callback invocation.
void Http2Connection::h2_end_sse(int32_t stream_id) {
    if (closed_) return;
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.sse_mode) return;

    it->second.sse_ended = true;

    if (it->second.sse_deferred) {
        it->second.sse_deferred = false;
        nghttp2_session_resume_data(session_, stream_id);
    }
    flush();
}

// ── WebSocket over HTTP/2 (RFC 8441) ──────────────────────────────────────────
//
// h2_begin_ws — called once by H2WSContext::begin() (from app.ws() wrapper).
// Sends `:status: 200` HEADERS (no END_STREAM), sets up the outgoing data
// provider, and wires the feed callback for incoming DATA frames.
//
void Http2Connection::h2_begin_ws(int32_t stream_id,
                                   std::function<void(const uint8_t*, size_t)> feed_cb) {
    if (closed_) return;
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    it->second.ws_mode      = true;
    it->second.ws_data_feed = std::move(feed_cb);

    // :status: 200 only — no WS-specific headers needed (RFC 8441 §4)
    nghttp2_nv status_nv;
    std::string status_val = "200";
    status_nv.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(":status"));
    status_nv.namelen  = 7;
    status_nv.value    = reinterpret_cast<uint8_t*>(status_val.data());
    status_nv.valuelen = status_val.size();
    status_nv.flags    = NGHTTP2_NV_FLAG_NONE;

    // Data provider — same deferred-queue pattern as SSE.
    nghttp2_data_provider prd{};
    prd.read_callback = [](nghttp2_session*, int32_t sid,
                            uint8_t* buf, size_t len,
                            uint32_t* flags,
                            nghttp2_data_source* /*src*/,
                            void* user_data) -> ssize_t {
        auto* self = static_cast<Http2Connection*>(user_data);
        auto it2 = self->streams_.find(sid);
        if (it2 == self->streams_.end()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        auto& s = it2->second;
        if (s.ws_pending.empty()) {
            if (s.ws_ended) { *flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
            s.ws_deferred = true;
            return NGHTTP2_ERR_DEFERRED;
        }
        size_t written = 0;
        while (!s.ws_pending.empty() && written < len) {
            auto& chunk = s.ws_pending.front();
            size_t n = std::min(chunk.size(), len - written);
            std::memcpy(buf + written, chunk.data(), n);
            written += n;
            if (n == chunk.size()) {
                s.ws_pending.erase(s.ws_pending.begin());
            } else {
                chunk.erase(0, n);
                break;
            }
        }
        s.ws_deferred = false;
        if (s.ws_pending.empty() && s.ws_ended)
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<ssize_t>(written);
    };

    nghttp2_submit_response(session_, stream_id, &status_nv, 1, &prd);
    flush();
}

// h2_push_ws — enqueue outgoing WS frame bytes and resume the data provider.
void Http2Connection::h2_push_ws(int32_t stream_id, std::string frame) {
    if (closed_) return;
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.ws_mode) return;

    it->second.ws_pending.push_back(std::move(frame));

    if (it->second.ws_deferred) {
        it->second.ws_deferred = false;
        nghttp2_session_resume_data(session_, stream_id);
    }
    flush();
}

// h2_end_ws — called when the WSConnection handler exits (or close() is called).
void Http2Connection::h2_end_ws(int32_t stream_id) {
    if (closed_) return;
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.ws_mode) return;

    it->second.ws_ended = true;

    if (it->second.ws_deferred) {
        it->second.ws_deferred = false;
        nghttp2_session_resume_data(session_, stream_id);
    }
    flush();
}

// ── Close ─────────────────────────────────────────────────────────────────────

void Http2Connection::close() {
    if (closed_) return;
    closed_ = true;

    // Cancel all in-flight stream coroutines
    for (auto& [id, s] : streams_)
        if (s.cancel_token) s.cancel_token->cancel();
    streams_.clear();  // destroys all BodySrc unique_ptrs

    if (session_) { nghttp2_session_del(session_); session_ = nullptr; }
    loop_.remove(fd_);
    if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
    ::close(fd_);
    if (conn_count_) conn_count_->fetch_sub(1, std::memory_order_relaxed);
}

// ── nghttp2 callbacks ─────────────────────────────────────────────────────────

ssize_t Http2Connection::send_cb(nghttp2_session*, const uint8_t* data,
                                  size_t length, int, void* user_data) {
    auto* self = static_cast<Http2Connection*>(user_data);
    self->write_buf_.append(reinterpret_cast<const char*>(data), length);
    return static_cast<ssize_t>(length);
}

int Http2Connection::on_begin_headers_cb(nghttp2_session*,
                                          const nghttp2_frame* frame,
                                          void* user_data) {
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;

    auto* self = static_cast<Http2Connection*>(user_data);
    self->streams_[frame->hd.stream_id] = {};
    return 0;
}

int Http2Connection::on_header_cb(nghttp2_session*,
                                   const nghttp2_frame* frame,
                                   const uint8_t* name,   size_t namelen,
                                   const uint8_t* value,  size_t valuelen,
                                   uint8_t, void* user_data) {
    auto* self = static_cast<Http2Connection*>(user_data);
    auto it = self->streams_.find(frame->hd.stream_id);
    if (it == self->streams_.end()) return 0;

    std::string n(reinterpret_cast<const char*>(name),  namelen);
    std::string v(reinterpret_cast<const char*>(value), valuelen);

    auto& s = it->second;
    if      (n == ":method")    s.method    = std::move(v);
    else if (n == ":path")      s.path      = std::move(v);
    else if (n == ":authority") s.authority = std::move(v);
    else if (n == ":scheme")    {} // ignore
    else if (n == ":protocol" && v == "websocket") s.ws_protocol = true;
    else                        s.req_headers[std::move(n)] = std::move(v);
    return 0;
}

int Http2Connection::on_data_chunk_recv_cb(nghttp2_session*, uint8_t,
                                            int32_t stream_id,
                                            const uint8_t* data, size_t len,
                                            void* user_data) {
    auto* self = static_cast<Http2Connection*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) return 0;

    if (it->second.ws_data_feed) {
        // WS mode: route raw bytes to the WSState frame parser
        it->second.ws_data_feed(data, len);
    } else {
        // Normal mode: accumulate request body
        it->second.body.append(reinterpret_cast<const char*>(data), len);
    }
    return 0;
}

int Http2Connection::on_frame_recv_cb(nghttp2_session*,
                                       const nghttp2_frame* frame,
                                       void* user_data) {
    int32_t stream_id = frame->hd.stream_id;
    // Only react to client-originated frames (odd stream IDs)
    if (stream_id <= 0 || (stream_id & 1) == 0) return 0;

    auto* self = static_cast<Http2Connection*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end() || it->second.dispatched) return 0;

    // RFC 8441 WebSocket: CONNECT+websocket HEADERS arrive without END_STREAM.
    // Dispatch immediately after the HEADERS frame.
    if (frame->hd.type == NGHTTP2_HEADERS &&
        it->second.ws_protocol &&
        it->second.method == "CONNECT") {
        it->second.dispatched = true;
        self->dispatch_stream(stream_id, it->second);
        return 0;
    }

    // Normal dispatch: wait for END_STREAM
    // • HEADERS with END_STREAM  → no body (GET, HEAD, DELETE…)
    // • DATA   with END_STREAM   → body fully received
    if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) return 0;

    it->second.dispatched = true;
    self->dispatch_stream(stream_id, it->second);
    return 0;
}

int Http2Connection::on_stream_close_cb(nghttp2_session*, int32_t stream_id,
                                         uint32_t, void* user_data) {
    auto* self = static_cast<Http2Connection*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end()) {
        if (it->second.cancel_token) it->second.cancel_token->cancel();
        // Notify a suspended WSState::recv() awaitable so it unblocks.
        // The feed callback holds no state we need to clean up — just clear it.
        it->second.ws_data_feed = nullptr;
        self->streams_.erase(it);
    }
    return 0;
}

} // namespace osodio::http
