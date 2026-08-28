#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <vector>
#include <nlohmann/json.hpp>
#include <jinja2cpp/template_env.h>
#include <jinja2cpp/filesystem_handler.h>
#include <jinja2cpp/value.h>
#include "cookies.hpp"

namespace osodio {

class Response {
    struct State {
        int         status_code = 200;
        std::string body;
        std::string templates_dir = "./templates";
        std::unordered_map<std::string, std::string> headers;
        // Set-Cookie is the one HTTP response header that legally appears
        // multiple times — keep them in a separate list so they survive the
        // map serialisation.
        std::vector<std::string> cookies;
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

    // Descarta el cuerpo ya escrito para que pueda escribirse otro.
    //
    // Existe para los manejadores de error: cuando uno se dispara, el cuerpo
    // por defecto ya esta comprometido, asi que sin esto cualquier res.json()
    // o res.render() del manejador se descartaria en silencio.
    //
    // No toca el codigo de estado ni las cabeceras, y es un no-op sobre una
    // respuesta en modo SSE o WebSocket, donde ya se enviaron bytes al socket.
    Response& reset_body() {
        if (state_->sse_started || state_->ws_started) return *this;
        state_->body.clear();
        state_->sendfile_path.clear();
        state_->sendfile_size = 0;
        state_->body_committed = false;
        return *this;
    }

    Response& header(std::string key, std::string value) {
        // Strip CR/LF from both key and value to prevent HTTP response splitting.
        auto strip_crlf = [](std::string& s) {
            s.erase(std::remove_if(s.begin(), s.end(),
                [](char c){ return c == '\r' || c == '\n'; }), s.end());
        };
        strip_crlf(key);
        strip_crlf(value);
        state_->headers[std::move(key)] = std::move(value);
        return *this;
    }

    // Set a cookie.  Multiple cookies can be set on one response — each emits
    // its own Set-Cookie header (the map storage in headers_ only allows one).
    //
    //   res.cookie("session", token, {.secure = true, .same_site = SameSite::Strict});
    //
    Response& cookie(std::string name, std::string value, CookieOptions opts = {}) {
        state_->cookies.push_back(
            build_set_cookie(std::move(name), std::move(value), std::move(opts)));
        return *this;
    }

    // Convenience: delete a cookie by name (sets Max-Age=0).
    Response& clear_cookie(std::string name, CookieOptions opts = {}) {
        opts.max_age = 0;
        state_->cookies.push_back(
            build_set_cookie(std::move(name), "", std::move(opts)));
        return *this;
    }

    Response& text(std::string body) {
        if (state_->body_committed) {
            std::cerr << "[osodio] Response.text() called after body already committed — ignoring\n";
            return *this;
        }
        header("Content-Type", "text/plain; charset=utf-8");
        state_->body = std::move(body);
        state_->body_committed = true;
        return *this;
    }

    Response& html(const std::string& content) {
        if (state_->body_committed) {
            std::cerr << "[osodio] Response.html() called after body already committed — ignoring\n";
            return *this;
        }
        header("Content-Type", "text/html; charset=utf-8");
        state_->body_committed = true;
        if (is_template_name(content)) {
            namespace fs = std::filesystem;
            // Reject traversal/absolute paths: html("../../etc/passwd.html") would
            // otherwise read arbitrary files reachable from the templates dir.
            fs::path requested(content);
            if (requested.is_absolute()) {
                state_->status_code = 403;
                state_->body = R"({"error":"Forbidden"})";
                state_->headers["Content-Type"] = "application/json; charset=utf-8";
                return *this;
            }
            for (const auto& comp : requested) {
                if (comp == "..") {
                    state_->status_code = 403;
                    state_->body = R"({"error":"Forbidden"})";
                    state_->headers["Content-Type"] = "application/json; charset=utf-8";
                    return *this;
                }
            }
            fs::path path = fs::path(state_->templates_dir) / requested;
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                std::cerr << "[osodio] template not found: " << path.string() << '\n';
                state_->status_code = 500;
                state_->body = R"({"error":"Internal Server Error"})";
                state_->headers["Content-Type"] = "application/json; charset=utf-8";
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
        if (state_->body_committed) {
            std::cerr << "[osodio] Response.json() called after body already committed — ignoring\n";
            return *this;
        }
        header("Content-Type", "application/json; charset=utf-8");
        state_->body = j.dump();
        state_->body_committed = true;
        return *this;
    }

    Response& send(std::string body) {
        if (state_->body_committed) {
            std::cerr << "[osodio] Response.send() called after body already committed — ignoring\n";
            return *this;
        }
        state_->body = std::move(body);
        state_->body_committed = true;
        return *this;
    }

    // Render a Jinja2 template from the templates directory.
    // Uses Jinja2Cpp; the TemplateEnv is cached per thread per templates_dir.
    //
    //   res.render("index.html", {{"user", user_data}, {"items", items}});
    //
    Response& render(const std::string& template_name,
                     const nlohmann::json& data = {}) {
        if (state_->body_committed) {
            std::cerr << "[osodio] Response.render() called after body already committed — ignoring\n";
            return *this;
        }
        // Reject traversal/absolute paths before handing to Jinja2Cpp.
        {
            std::filesystem::path requested(template_name);
            if (requested.is_absolute()) {
                state_->status_code = 403;
                state_->body = R"({"error":"Forbidden"})";
                state_->headers["Content-Type"] = "application/json; charset=utf-8";
                state_->body_committed = true;
                return *this;
            }
            for (const auto& comp : requested) {
                if (comp == "..") {
                    state_->status_code = 403;
                    state_->body = R"({"error":"Forbidden"})";
                    state_->headers["Content-Type"] = "application/json; charset=utf-8";
                    state_->body_committed = true;
                    return *this;
                }
            }
        }
        header("Content-Type", "text/html; charset=utf-8");
        // One TemplateEnv per (thread × templates_dir): Jinja2Cpp caches parsed
        // templates inside the environment, same pattern as the old inja setup.
        thread_local std::unordered_map<std::string,
                                        std::unique_ptr<jinja2::TemplateEnv>> envs;
        auto it = envs.find(state_->templates_dir);
        if (it == envs.end()) {
            auto env = std::make_unique<jinja2::TemplateEnv>();
            env->AddFilesystemHandler(
                "", std::make_shared<jinja2::RealFileSystem>(state_->templates_dir));
            it = envs.emplace(state_->templates_dir, std::move(env)).first;
        }
        state_->body_committed = true;
        auto tmpl = it->second->LoadTemplate(template_name);
        if (!tmpl) {
            std::cerr << "[osodio] template load error: "
                      << tmpl.error().ToString() << '\n';
            state_->status_code = 500;
            state_->body = R"({"error":"Internal Server Error"})";
            state_->headers["Content-Type"] = "application/json; charset=utf-8";
            return *this;
        }
        auto result = tmpl->RenderAsString(json_to_values_map(data));
        if (!result) {
            std::cerr << "[osodio] template render error: "
                      << result.error().ToString() << '\n';
            state_->status_code = 500;
            state_->body = R"({"error":"Internal Server Error"})";
            state_->headers["Content-Type"] = "application/json; charset=utf-8";
            return *this;
        }
        state_->body = std::move(result.value());
        return *this;
    }

    // Zero-copy static file: instead of reading the file into the body,
    // record the path and let the connection layer use sendfile(2).
    // Content-Type should be set by the caller before calling send_file().
    //
    // WARNING: if `path` is derived from user input, use serve_file_from()
    // instead — it enforces a root directory and resolves symlinks safely.
    Response& send_file(const std::filesystem::path& path) {
        // Reject paths with ".." components to prevent directory traversal.
        // Internal callers (serve_file_from, try_serve_static) pass canonical
        // paths so this check is a no-op for them.
        for (const auto& comp : path) {
            if (comp == "..") {
                state_->status_code = 403;
                state_->body = R"({"error":"Forbidden"})";
                state_->headers["Content-Type"] = "application/json; charset=utf-8";
                return *this;
            }
        }
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

    // Safe file serving with path-traversal and symlink protection.
    // Fully resolves root/user_path (following all symlinks) and rejects
    // anything that resolves outside root.  Use this instead of send_file()
    // when user_path comes from request input.
    //
    //   res.serve_file_from("./uploads", req.param("name").value_or(""));
    //
    Response& serve_file_from(const std::filesystem::path& root,
                               const std::filesystem::path& user_path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        // canonical() resolves ALL symlinks (unlike weakly_canonical), which
        // prevents symlink-swap attacks where a symlink inside root points to
        // a file outside root.
        auto canonical_root = fs::canonical(root, ec);
        if (ec) {
            state_->status_code = 500;
            state_->body = R"({"error":"Internal Server Error"})";
            state_->headers["Content-Type"] = "application/json; charset=utf-8";
            return *this;
        }
        auto canonical_file = fs::canonical(canonical_root / user_path, ec);
        if (ec) {
            state_->status_code = 404;
            state_->body = R"({"error":"Not Found"})";
            state_->headers["Content-Type"] = "application/json; charset=utf-8";
            return *this;
        }
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
    const std::vector<std::string>&
                           cookies()        const { return state_->cookies; }
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
        for (const auto& c : state_->cookies)
            os << "Set-Cookie: " << c << "\r\n";
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
        for (const auto& c : state_->cookies)
            os << "Set-Cookie: " << c << "\r\n";
        os << "\r\n";
        // Body only for normal (non-sendfile) responses
        if (state_->sendfile_path.empty())
            os << state_->body;
        return os.str();
    }

private:
    static jinja2::Value json_to_value(const nlohmann::json& j) {
        if (j.is_null())             return {};
        if (j.is_boolean())          return j.get<bool>();
        if (j.is_number_integer())   return j.get<int64_t>();
        if (j.is_number_float())     return j.get<double>();
        if (j.is_string())           return j.get<std::string>();
        if (j.is_array()) {
            jinja2::ValuesList list;
            list.reserve(j.size());
            for (const auto& item : j) list.push_back(json_to_value(item));
            return list;
        }
        // object
        jinja2::ValuesMap map;
        for (auto& [k, v] : j.items()) map[k] = json_to_value(v);
        return map;
    }

    static jinja2::ValuesMap json_to_values_map(const nlohmann::json& j) {
        jinja2::ValuesMap map;
        if (j.is_object())
            for (auto& [k, v] : j.items()) map[k] = json_to_value(v);
        return map;
    }

    static bool is_template_name(const std::string& s) {
        if (s.empty() || s.find('\n') != std::string::npos) return false;
        if (s.find('<') != std::string::npos) return false;
        return s.ends_with(".html") || s.ends_with(".htm");
    }

    static const char* reason_phrase(int code) noexcept {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 307: return "Temporary Redirect";
            case 308: return "Permanent Redirect";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 409: return "Conflict";
            case 410: return "Gone";
            case 413: return "Content Too Large";
            case 415: return "Unsupported Media Type";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            default:  return "Unknown";
        }
    }
};

} // namespace osodio
