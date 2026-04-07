#include "http_parser.hpp"
#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace osodio::http {

// ── Per-connection parse state ─────────────────────────────────────────────

struct HttpParser::ParseContext {
    ParsedRequest current;

    // Header accumulation
    std::string last_field;
    std::string last_value;
    bool        value_pending = false;  // last_value not yet committed to map
    size_t      header_count  = 0;

    // Flag set by any callback that detects a limit violation
    bool error = false;

    // Back-pointer to the owning parser's OnComplete (stable address)
    OnComplete* on_complete = nullptr;
};

// ── llhttp callbacks ───────────────────────────────────────────────────────

static ParseContext* ctx(llhttp_t* p) {
    return static_cast<ParseContext*>(p->data);
}

static int cb_on_url(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->current.path.size() + len > kMaxUrlSize) { c->error = true; return HPE_USER; }
    c->current.path.append(at, len);
    return HPE_OK;
}

static void commit_header(ParseContext* c) {
    if (!c->value_pending) return;
    // Lowercase the field name for case-insensitive lookup
    std::string key = c->last_field;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    c->current.headers[std::move(key)] = std::move(c->last_value);
    c->last_field.clear();
    c->last_value.clear();
    c->value_pending = false;
    ++c->header_count;
}

static int cb_on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    // A new field name means the previous field+value pair is complete
    if (c->value_pending) {
        if (c->header_count >= kMaxHeaderCount) { c->error = true; return HPE_USER; }
        commit_header(c);
    }
    if (c->last_field.size() + len > kMaxHeaderSize) { c->error = true; return HPE_USER; }
    c->last_field.append(at, len);
    return HPE_OK;
}

static int cb_on_header_value(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->last_value.size() + len > kMaxHeaderSize) { c->error = true; return HPE_USER; }
    c->last_value.append(at, len);
    c->value_pending = true;
    return HPE_OK;
}

static int cb_on_headers_complete(llhttp_t* p) {
    auto* c = ctx(p);
    // Commit the final header
    if (c->value_pending) {
        if (c->header_count >= kMaxHeaderCount) { c->error = true; return HPE_USER; }
        commit_header(c);
    }
    // HTTP version
    int major = llhttp_get_http_major(p);
    int minor = llhttp_get_http_minor(p);
    c->current.version = (major == 1 && minor == 0) ? "HTTP/1.0" : "HTTP/1.1";
    return HPE_OK;
}

static int cb_on_body(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->current.body.size() + len > kMaxBodySize) { c->error = true; return HPE_USER; }
    c->current.body.append(at, len);
    return HPE_OK;
}

static int cb_on_message_complete(llhttp_t* p) {
    auto* c = ctx(p);

    // Split path from query string
    auto q = c->current.path.find('?');
    if (q != std::string::npos) {
        c->current.query = c->current.path.substr(q + 1);
        c->current.path  = c->current.path.substr(0, q);
    }

    // Method name
    c->current.method = llhttp_method_name(
        static_cast<llhttp_method_t>(llhttp_get_method(p)));

    (*c->on_complete)(std::move(c->current));

    // Reset per-message state (keep parser alive for keep-alive)
    c->current      = {};
    c->header_count = 0;

    return HPE_OK;
}

// ── HttpParser ─────────────────────────────────────────────────────────────

HttpParser::HttpParser(OnComplete on_complete)
    : on_complete_(std::move(on_complete))
    , ctx_(std::make_unique<ParseContext>())
    , parser_(std::make_unique<llhttp_t>())
    , settings_(std::make_unique<llhttp_settings_t>())
{
    ctx_->on_complete = &on_complete_;

    llhttp_settings_init(settings_.get());
    settings_->on_url              = cb_on_url;
    settings_->on_header_field     = cb_on_header_field;
    settings_->on_header_value     = cb_on_header_value;
    settings_->on_headers_complete = cb_on_headers_complete;
    settings_->on_body             = cb_on_body;
    settings_->on_message_complete = cb_on_message_complete;

    llhttp_init(parser_.get(), HTTP_REQUEST, settings_.get());
    parser_->data = ctx_.get();
}

HttpParser::~HttpParser() = default;

bool HttpParser::feed(const char* data, size_t len) {
    if (ctx_->error) return false;
    llhttp_errno_t err = llhttp_execute(parser_.get(), data, static_cast<size_t>(len));
    return (err == HPE_OK) && !ctx_->error;
}

void HttpParser::reset() {
    ctx_->current      = {};
    ctx_->last_field.clear();
    ctx_->last_value.clear();
    ctx_->value_pending = false;
    ctx_->header_count  = 0;
    ctx_->error         = false;
    llhttp_reset(parser_.get());
}

} // namespace osodio::http
