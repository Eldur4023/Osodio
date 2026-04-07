#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstddef>

// Forward-declare llhttp types to avoid pulling the header into every TU
// that includes this file.
struct llhttp__internal_s;
typedef struct llhttp__internal_s llhttp_t;
struct llhttp_settings_s;
typedef struct llhttp_settings_s llhttp_settings_t;

namespace osodio::http {

// Parsed representation of one complete HTTP/1.1 request.
struct ParsedRequest {
    std::string method;
    std::string path;
    std::string query;    // raw query string
    std::string version;  // "HTTP/1.1" or "HTTP/1.0"
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

// ── Security limits ────────────────────────────────────────────────────────
inline constexpr size_t kMaxUrlSize     =  8 * 1024;   //  8 KB
inline constexpr size_t kMaxHeaderSize  =  8 * 1024;   //  8 KB per field/value
inline constexpr size_t kMaxHeaderCount =  100;
inline constexpr size_t kMaxBodySize    = 16 * 1024 * 1024; // 16 MB

// Incremental HTTP/1.1 request parser backed by llhttp.
//
// Feed it bytes as they arrive from the socket.  When a complete request is
// parsed, on_complete is called.  The parser resets automatically for the next
// request on the same keep-alive connection.
class HttpParser {
public:
    using OnComplete = std::function<void(ParsedRequest)>;

    explicit HttpParser(OnComplete on_complete);
    ~HttpParser();

    // Returns false on a parse error (caller should close the connection).
    bool feed(const char* data, size_t len);

    void reset();

private:
    struct ParseContext;

    OnComplete                       on_complete_;
    std::unique_ptr<ParseContext>    ctx_;
    std::unique_ptr<llhttp_t>        parser_;
    std::unique_ptr<llhttp_settings_t> settings_;
};

} // namespace osodio::http
