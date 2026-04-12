#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <memory>
#include <array>
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <coroutine>
#include <osodio/core/event_loop.hpp>
#include "cancel.hpp"

namespace osodio {

// ─── WSMessage ────────────────────────────────────────────────────────────────

struct WSMessage {
    enum class Opcode : uint8_t {
        Continuation = 0x0,
        Text         = 0x1,
        Binary       = 0x2,
        Close        = 0x8,
        Ping         = 0x9,
        Pong         = 0xA,
    };

    Opcode      opcode = Opcode::Text;
    std::string data;

    bool is_text()   const { return opcode == Opcode::Text; }
    bool is_binary() const { return opcode == Opcode::Binary; }
    bool is_close()  const { return opcode == Opcode::Close; }
    bool is_ping()   const { return opcode == Opcode::Ping; }
    bool is_pong()   const { return opcode == Opcode::Pong; }
};

// ─── Internal detail ─────────────────────────────────────────────────────────

namespace detail {

// ── SHA-1 (RFC 3174) — needed for the WebSocket handshake ────────────────────

inline void sha1_compress(uint32_t s[5], const uint8_t blk[64]) noexcept {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(blk[i*4])<<24)|(uint32_t(blk[i*4+1])<<16)
              |(uint32_t(blk[i*4+2])<<8)| uint32_t(blk[i*4+3]);
    for (int i = 16; i < 80; ++i) {
        uint32_t t = w[i-3]^w[i-8]^w[i-14]^w[i-16];
        w[i] = (t<<1)|(t>>31);
    }
    uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f,k;
        if      (i<20){ f=(b&c)|(~b&d);          k=0x5A827999u; }
        else if (i<40){ f=b^c^d;                  k=0x6ED9EBA1u; }
        else if (i<60){ f=(b&c)|(b&d)|(c&d);      k=0x8F1BBCDCu; }
        else          { f=b^c^d;                  k=0xCA62C1D6u; }
        uint32_t tmp = ((a<<5)|(a>>27))+f+e+k+w[i];
        e=d; d=c; c=(b<<30)|(b>>2); b=a; a=tmp;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e;
}

inline std::array<uint8_t,20> sha1(const std::string& msg) noexcept {
    uint32_t s[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    std::string p = msg;
    p += char(0x80);
    while (p.size()%64 != 56) p += char(0);
    uint64_t bits = uint64_t(msg.size())*8;
    for (int i = 7; i >= 0; --i) p += char((bits>>(i*8))&0xFF);
    for (size_t i = 0; i < p.size(); i += 64)
        sha1_compress(s, reinterpret_cast<const uint8_t*>(p.data()+i));
    std::array<uint8_t,20> h;
    for (int i = 0; i < 5; ++i) {
        h[i*4]=(s[i]>>24)&0xFF; h[i*4+1]=(s[i]>>16)&0xFF;
        h[i*4+2]=(s[i]>>8)&0xFF; h[i*4+3]=s[i]&0xFF;
    }
    return h;
}

// ── Base64 encode ─────────────────────────────────────────────────────────────

inline std::string base64(const uint8_t* d, size_t n) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n+2)/3)*4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = uint32_t(d[i])<<16
                   | (i+1<n ? uint32_t(d[i+1])<<8 : 0)
                   | (i+2<n ? uint32_t(d[i+2])    : 0);
        out += t[(b>>18)&63];
        out += t[(b>>12)&63];
        out += (i+1<n) ? t[(b>>6)&63] : '=';
        out += (i+2<n) ? t[ b    &63] : '=';
    }
    return out;
}

// ── WebSocket accept key (RFC 6455 §4.2.2) ───────────────────────────────────

inline std::string ws_accept(const std::string& client_key) {
    auto h = sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(h.data(), h.size());
}

// ── Frame builder (server→client; always unmasked) ───────────────────────────

inline std::string build_frame(std::string_view payload,
                                uint8_t opcode,
                                bool fin = true) {
    std::string f;
    f.reserve(payload.size() + 10);
    f += char((fin ? 0x80 : 0x00) | opcode);
    if      (payload.size() < 126)   { f += char(payload.size()); }
    else if (payload.size() < 65536) {
        f += char(126);
        f += char(payload.size()>>8);
        f += char(payload.size()&0xFF);
    } else {
        f += char(127);
        for (int i = 7; i >= 0; --i)
            f += char((payload.size()>>(i*8))&0xFF);
    }
    f.append(payload);
    return f;
}

// ── WSState — shared between WSConnection and HttpConnection ─────────────────

struct WSState {
    int              fd   = -1;
    std::weak_ptr<CancellationToken> token;
    core::EventLoop* loop = nullptr;

    // HTTP/2 path: instead of writing to fd, frames are enqueued via this
    // callback (set by app.ws() when _h2_ws_ctx is present).
    std::function<void(std::string)> send_fn;

    std::string          read_buf;
    std::vector<WSMessage> pending;

    // For reassembling fragmented messages
    std::string  frag_buf;
    uint8_t      frag_opcode = 0;

    std::coroutine_handle<> recv_waiter;
    bool closed = false;

    // ── raw_send — routes through send_fn (H2) or ::write(fd) (HTTP/1.1) ─────
    void raw_send(const std::string& frame) {
        if (send_fn) { send_fn(frame); return; }
        size_t w = 0;
        while (w < frame.size()) {
            ssize_t n = ::write(fd, frame.data()+w, frame.size()-w);
            if (n <= 0) break;
            w += static_cast<size_t>(n);
        }
    }

    // ── Parse as many complete frames as possible ─────────────────────────────
    void try_parse() {
        while (true) {
            if (read_buf.size() < 2) break;
            auto* p = reinterpret_cast<const uint8_t*>(read_buf.data());

            bool    fin     = (p[0] & 0x80) != 0;
            uint8_t opcode  = p[0] & 0x0F;
            bool    masked  = (p[1] & 0x80) != 0;
            uint8_t len7    = p[1] & 0x7F;

            size_t  hdr = 2;
            uint64_t payload_len;
            if      (len7 == 126) { if (read_buf.size()<4) break; payload_len=(uint8_t(p[2])<<8)|uint8_t(p[3]); hdr+=2; }
            else if (len7 == 127) { if (read_buf.size()<10) break; payload_len=0; for(int i=0;i<8;++i) payload_len=(payload_len<<8)|p[2+i]; hdr+=8; }
            else                  { payload_len = len7; }

            if (masked) hdr += 4;
            if (read_buf.size() < hdr + payload_len) break;  // wait for more data

            uint8_t mask_key[4] = {};
            if (masked) memcpy(mask_key, p + hdr - 4, 4);

            std::string payload(read_buf.data() + hdr, payload_len);
            if (masked)
                for (size_t i = 0; i < payload.size(); ++i)
                    payload[i] ^= mask_key[i % 4];

            read_buf.erase(0, hdr + payload_len);

            // Control frames (RFC 6455 §5.5): never fragmented
            if (opcode == 0x8) {  // Close
                closed = true;
                raw_send(std::string("\x88\x00", 2));  // echo close frame
                pending.push_back({WSMessage::Opcode::Close, std::move(payload)});
                return;
            }
            if (opcode == 0x9) {  // Ping → auto-pong
                auto pong = build_frame(payload, 0xA);
                raw_send(pong);
                continue;
            }
            if (opcode == 0xA) {  // Pong
                pending.push_back({WSMessage::Opcode::Pong, std::move(payload)});
                continue;
            }

            // Data frames — handle fragmentation
            if (opcode == 0x0) {  // Continuation
                frag_buf += payload;
                if (fin) {
                    pending.push_back({WSMessage::Opcode(frag_opcode), std::move(frag_buf)});
                    frag_buf.clear();
                }
            } else {  // Text or Binary
                if (!fin) {
                    frag_opcode = opcode;
                    frag_buf    = std::move(payload);
                } else {
                    pending.push_back({WSMessage::Opcode(opcode), std::move(payload)});
                }
            }
        }
    }

    // ── Called by HttpConnection::do_read() when in WS mode (HTTP/1.1 only) ──
    void on_readable() {
        char buf[65536];
        while (true) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) break;
                closed = true;
                break;
            }
            read_buf.append(buf, n);
        }
        try_parse();
        resume_waiter_if_ready();
    }

    // ── Called by Http2Connection::on_data_chunk_recv_cb (HTTP/2 only) ───────
    void feed(const uint8_t* data, size_t len) {
        read_buf.append(reinterpret_cast<const char*>(data), len);
        try_parse();
        resume_waiter_if_ready();
    }

    void resume_waiter_if_ready() {
        if (!recv_waiter) return;
        if (pending.empty() && !closed) return;
        auto h = std::exchange(recv_waiter, {});
        if (auto t = token.lock()) t->clear_wake();
        if (!h.done()) h.resume();
    }

    // Called by HttpConnection::close() — wakes any suspended recv()
    void notify_closed() {
        closed = true;
        if (recv_waiter) {
            auto h = std::exchange(recv_waiter, {});
            if (auto t = token.lock()) t->clear_wake();
            if (!h.done()) h.resume();
        }
    }
};

} // namespace detail

// ─── WSConnection ─────────────────────────────────────────────────────────────
//
// Handle to an open WebSocket connection.  Passed by value to ws() handlers.
//
//   app.ws("/echo", [](WSConnection ws) -> Task<void> {
//       while (ws.is_open()) {
//           auto msg = co_await ws.recv();
//           if (!msg) break;           // connection closed
//           if (msg->is_text()) ws.send("echo: " + msg->data);
//       }
//   });

class WSConnection {
public:
    explicit WSConnection(std::shared_ptr<detail::WSState> s) : s_(std::move(s)) {}
    WSConnection(WSConnection&&) = default;
    WSConnection& operator=(WSConnection&&) = default;
    WSConnection(const WSConnection&) = delete;

    // ── Awaitable: suspends until a message arrives or the connection closes ──
    struct RecvAwaitable {
        std::shared_ptr<detail::WSState> s;

        bool await_ready() noexcept {
            return !s->pending.empty() || s->closed;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            s->recv_waiter = h;
            // Register with the CancellationToken so close() wakes us early.
            if (auto t = s->token.lock()) {
                t->set_wake([h, sp = s.get()]() mutable {
                    sp->closed      = true;
                    sp->recv_waiter = {};
                    if (!h.done()) h.resume();
                });
            }
        }

        std::optional<WSMessage> await_resume() noexcept {
            if (!s->pending.empty()) {
                auto msg = std::move(s->pending.front());
                s->pending.erase(s->pending.begin());
                return msg;
            }
            return std::nullopt;  // connection closed
        }
    };

    RecvAwaitable recv() { return RecvAwaitable{s_}; }

    // ── Send helpers ──────────────────────────────────────────────────────────

    bool send(std::string_view text) {
        return write_frame(text, 0x1);
    }

    bool send_binary(const void* data, size_t len) {
        return write_frame(std::string_view(static_cast<const char*>(data), len), 0x2);
    }

    // Send a Ping frame.  The remote end should reply with Pong automatically.
    bool ping(std::string_view payload = "") {
        return write_frame(payload, 0x9);
    }

    // Send a Close frame and mark the connection as closed.
    void close(uint16_t code = 1000) {
        if (!s_ || s_->closed) return;
        s_->closed = true;
        char frame[4];
        frame[0] = char(0x88);   // FIN + opcode Close
        frame[1] = 2;
        frame[2] = char(code >> 8);
        frame[3] = char(code & 0xFF);
        s_->raw_send(std::string(frame, 4));
    }

    bool is_open() const {
        return s_ && !s_->closed
               && !(s_->token.expired()
                    || (s_->token.lock() && s_->token.lock()->is_cancelled()));
    }

private:
    std::shared_ptr<detail::WSState> s_;

    bool write_frame(std::string_view payload, uint8_t opcode) {
        if (!is_open()) return false;
        auto frame = detail::build_frame(payload, opcode);
        if (s_->send_fn) {
            s_->send_fn(frame);
            return true;
        }
        size_t w = 0;
        while (w < frame.size()) {
            ssize_t n = ::write(s_->fd, frame.data()+w, frame.size()-w);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // drop (lossy)
                s_->closed = true;
                return false;
            }
            w += static_cast<size_t>(n);
        }
        return true;
    }
};

} // namespace osodio
