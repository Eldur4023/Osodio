#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <cstddef>

namespace osodio::http {

// Parsed representation of one complete HTTP/1.1 request.
struct ParsedRequest {
    std::string method;
    std::string path;
    std::string query;    // raw query string, e.g. "page=1&limit=20"
    std::string version;  // "HTTP/1.1" or "HTTP/1.0"
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

// Incremental HTTP/1.1 request parser.
//
// Feed it bytes as they arrive from the socket. When a complete request
// is parsed, on_complete is called. The parser then resets automatically,
// ready for the next request on the same keep-alive connection.
class HttpParser {
public:
    using OnComplete = std::function<void(ParsedRequest)>;

    explicit HttpParser(OnComplete on_complete);

    // Feed raw bytes from the socket.
    // Returns false on a parse error (caller should close the connection).
    bool feed(const char* data, size_t len);

    void reset();

private:
    enum class State { REQUEST_LINE, HEADERS, BODY, ERROR };

    OnComplete  on_complete_;
    State       state_          = State::REQUEST_LINE;
    std::string buf_;               // accumulation buffer
    ParsedRequest current_;
    size_t      content_length_ = 0;

    bool process();
    bool parse_request_line(const std::string& line);
    void parse_header_line(const std::string& line);
    void emit_and_reset();
};

} // namespace osodio::http
