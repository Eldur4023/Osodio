#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <functional>
#include <unistd.h>
#include <cerrno>
#include <nlohmann/json.hpp>
#include <inja.hpp>

namespace osodio {

class Response {
    struct State {
        int         status_code = 200;
        std::string body;
        std::string templates_dir = "./templates";
        std::unordered_map<std::string, std::string> headers;
        // sendfile path: when set, build() emits only headers; the connection
        // uses sendfile(2) to stream the file body directly to the socket.
        std::string     sendfile_path;
        std::uintmax_t  sendfile_size = 0;

        // SSE / WebSocket mode: headers already written directly to the socket;
        // finish_dispatch must not send a second response.
        bool sse_started    = false;
        bool ws_started     = false;
        // Set when any body-writing method is first called.
        // Lets handlers and middlewares check res.is_committed() before writing.
        bool body_committed = false;
    };
    std::shared_ptr<State> state_;

public:
    Response() : state_(std::make_shared<State>()) {}

    // ── Builder methods ───────────────────────────────────────────────────────

    Response& status(int code) {
        state_->status_code = code;
        return *this;
    }

    Response& header(std::string key, std::string value) {
        state_->headers[std::move(key)] = std::move(value);
        return *this;
    }

    Response& text(std::string body) {
        header("Content-Type", "text/plain; charset=utf-8");
        state_->body = std::move(body);
        state_->body_committed = true;
        return *this;
    }

    Response& html(const std::string& content) {
        header("Content-Type", "text/html; charset=utf-8");
        state_->body_committed = true;
        if (is_template_name(content)) {
            namespace fs = std::filesystem;
            fs::path path = fs::path(state_->templates_dir) / content;
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                state_->status_code = 500;
                state_->body = "Template not found: " + path.string();
                return *this;
            }
            state_->body = std::string(std::istreambuf_iterator<char>(f),
                                       std::istreambuf_iterator<char>());
        } else {
            state_->body = content;
        }
        return *this;
    }

    Response& json(const nlohmann::json& j) {
        header("Content-Type", "application/json; charset=utf-8");
        state_->body = j.dump();
        state_->body_committed = true;
        return *this;
    }

    Response& send(std::string body) {
        state_->body = std::move(body);
        state_->body_committed = true;
        return *this;
    }

    // Render a Jinja2-compatible template from the templates directory.
    // Uses inja; the Environment is cached per thread per templates_dir.
    //
    //   res.render("index.html", {{"user", user_data}, {"items", items}});
    //
    Response& render(const std::string& template_name,
                     const nlohmann::json& data = {}) {
        header("Content-Type", "text/html; charset=utf-8");
        try {
            // One inja::Environment per (thread × templates_dir): templates are
            // parsed once and cached inside the Environment.
            thread_local std::unordered_map<std::string, inja::Environment> envs;
            auto it = envs.find(state_->templates_dir);
            if (it == envs.end()) {
                it = envs.emplace(
                    state_->templates_dir,
                    inja::Environment{state_->templates_dir + "/"}
                ).first;
            }
            state_->body_committed = true;
            state_->body = it->second.render_file(template_name, data);
        } catch (const std::exception& e) {
            state_->status_code = 500;
            state_->body = std::string("Template error: ") + e.what();
        }
        return *this;
    }

    // Zero-copy static file: instead of reading the file into the body,
    // record the path and let the connection layer use sendfile(2).
    // Content-Type should be set by the caller before calling send_file().
    Response& send_file(const std::filesystem::path& path) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) { state_->status_code = 500; state_->body = "Cannot stat file"; return *this; }
        state_->sendfile_path = path.string();
        state_->sendfile_size = sz;
        state_->body_committed = true;
        return *this;
    }

    // Overload for callers that already have the file size — skips the extra stat(2).
    Response& send_file(const std::filesystem::path& path, std::uintmax_t known_size) {
        state_->sendfile_path = path.string();
        state_->sendfile_size = known_size;
        state_->body_committed = true;
        return *this;
    }

    // Safe file serving with path-traversal protection.
    // Resolves root/user_path canonically and rejects anything that escapes root.
    // Use this instead of send_file() when user_path comes from request input.
    //
    //   res.serve_file_from("./uploads", req.param("name").value_or(""));
    //
    Response& serve_file_from(const std::filesystem::path& root,
                               const std::filesystem::path& user_path) {
        namespace fs = std::filesystem;
        auto canonical_root = fs::weakly_canonical(root);
        auto canonical_file = fs::weakly_canonical(canonical_root / user_path);
        auto [ri, fi] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                       canonical_file.begin());
        if (ri != canonical_root.end()) {
            state_->status_code = 403;
            state_->body = R"({"error":"Forbidden"})";
            state_->headers["Content-Type"] = "application/json; charset=utf-8";
            return *this;
        }
        return send_file(canonical_file);
    }




    // ── Framework-internal ───────────────────────────────────────────────────

    void set_templates_dir(const std::string& dir) { state_->templates_dir = dir; }

    int                    status_code()    const { return state_->status_code; }
    const std::string&     body()           const { return state_->body; }
    const std::unordered_map<std::string, std::string>&
                           headers_map()    const { return state_->headers; }
    const std::string&     sendfile_path()  const { return state_->sendfile_path; }
    std::uintmax_t         sendfile_size()  const { return state_->sendfile_size; }
    bool                   is_committed()   const { return state_->body_committed; }
    bool                   sse_started()    const { return state_->sse_started; }
    void                   mark_sse_started()    { state_->sse_started = true; }
    bool                   ws_started()     const { return state_->ws_started; }
    void                   mark_ws_started()     { state_->ws_started  = true; }
    std::string            content_type()   const {
        auto it = state_->headers.find("Content-Type");
        return (it != state_->headers.end()) ? it->second : "";
    }

    // Headers-only build for SSE: no Content-Length (streaming, length unknown).
    std::string build_sse_headers() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << state_->status_code
           << ' ' << reason_phrase(state_->status_code) << "\r\n";
        for (const auto& [k, v] : state_->headers)
            os << k << ": " << v << "\r\n";
        os << "\r\n";
        return os.str();
    }

    std::string build() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << state_->status_code
           << ' ' << reason_phrase(state_->status_code) << "\r\n";
        // Content-Length: use file size when sendfile is in play
        auto clen = state_->sendfile_path.empty()
                    ? state_->body.size()
                    : static_cast<std::size_t>(state_->sendfile_size);
        os << "Content-Length: " << clen << "\r\n";
        for (const auto& [k, v] : state_->headers)
            os << k << ": " << v << "\r\n";
        os << "\r\n";
        // Body only for normal (non-sendfile) responses
        if (state_->sendfile_path.empty())
            os << state_->body;
        return os.str();
    }

private:
    static bool is_template_name(const std::string& s) {
        if (s.empty() || s.find('\n') != std::string::npos) return false;
        if (s.find('<') != std::string::npos) return false;
        return s.ends_with(".html") || s.ends_with(".htm");
    }

    static const char* reason_phrase(int code) noexcept {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    }
};

} // namespace osodio
