#include "http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace osodio::http {

HttpParser::HttpParser(OnComplete on_complete)
    : on_complete_(std::move(on_complete)) {}

bool HttpParser::feed(const char* data, size_t len) {
    buf_.append(data, len);
    return process();
}

void HttpParser::reset() {
    state_          = State::REQUEST_LINE;
    buf_.clear();
    current_        = {};
    content_length_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Core parse loop
// ─────────────────────────────────────────────────────────────────────────────

bool HttpParser::process() {
    while (true) {
        if (state_ == State::ERROR) return false;

        // ── Body: wait until we have content_length_ bytes ──────────────────
        if (state_ == State::BODY) {
            if (buf_.size() < content_length_) return true;
            current_.body.assign(buf_, 0, content_length_);
            buf_.erase(0, content_length_);
            emit_and_reset();
            continue; // there might be a pipelined request in buf_
        }

        // ── Line-based states ────────────────────────────────────────────────
        size_t pos = buf_.find("\r\n");
        if (pos == std::string::npos) return true; // need more data

        std::string line = buf_.substr(0, pos);
        buf_.erase(0, pos + 2);

        if (state_ == State::REQUEST_LINE) {
            if (!parse_request_line(line)) {
                state_ = State::ERROR;
                return false;
            }
            state_ = State::HEADERS;

        } else { // HEADERS
            if (line.empty()) {
                // End of headers — decide whether there's a body
                auto it = current_.headers.find("content-length");
                if (it != current_.headers.end()) {
                    try {
                        content_length_ = std::stoull(it->second);
                    } catch (...) {
                        state_ = State::ERROR;
                        return false;
                    }
                }

                // Guard against unreasonably large bodies (16 MB limit)
                if (content_length_ > 16 * 1024 * 1024) {
                    state_ = State::ERROR;
                    return false;
                }

                if (content_length_ > 0) {
                    state_ = State::BODY;
                } else {
                    emit_and_reset();
                }
            } else {
                parse_header_line(line);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool HttpParser::parse_request_line(const std::string& line) {
    // Format: METHOD SP Request-URI SP HTTP-Version
    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;

    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    current_.method  = line.substr(0, sp1);
    current_.version = line.substr(sp2 + 1);

    std::string url = line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Split path from query string
    auto q = url.find('?');
    if (q != std::string::npos) {
        current_.path  = url.substr(0, q);
        current_.query = url.substr(q + 1);
    } else {
        current_.path = url;
    }

    return !current_.method.empty() && !current_.path.empty();
}

void HttpParser::parse_header_line(const std::string& line) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return;

    std::string key   = line.substr(0, colon);
    std::string value = line.substr(colon + 1);

    // Trim leading/trailing whitespace from value
    size_t start = value.find_first_not_of(" \t");
    size_t end   = value.find_last_not_of(" \t\r");
    if (start == std::string::npos) return;
    value = value.substr(start, end - start + 1);

    // Trim trailing whitespace from key
    while (!key.empty() && std::isspace((unsigned char)key.back())) key.pop_back();

    // Lowercase key for case-insensitive lookup
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    current_.headers[std::move(key)] = std::move(value);
}

void HttpParser::emit_and_reset() {
    on_complete_(std::move(current_));
    current_        = {};
    content_length_ = 0;
    state_          = State::REQUEST_LINE;
}

} // namespace osodio::http
