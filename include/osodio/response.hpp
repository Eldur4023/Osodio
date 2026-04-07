#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

namespace osodio {

class Response {
    struct State {
        int         status_code = 200;
        std::string body;
        std::string templates_dir = "./templates";
        std::unordered_map<std::string, std::string> headers;
        bool async = false;
        std::function<void()> on_complete_cb;
    };
    std::shared_ptr<State> state_;

public:
    Response() : state_(std::make_shared<State>()) {}

    // --- Builder methods ---

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
        return *this;
    }

    Response& html(const std::string& content) {
        header("Content-Type", "text/html; charset=utf-8");
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
        return *this;
    }

    Response& send(std::string body) {
        state_->body = std::move(body);
        return *this;
    }

    // --- Async support ---
    bool is_async() const { return state_->async; }
    void mark_async() { state_->async = true; }
    void unmark_async() { state_->async = false; }
    
    void on_complete(std::function<void()> cb) {
        state_->on_complete_cb = std::move(cb);
    }
    
    void complete_async() {
        if (state_->on_complete_cb) state_->on_complete_cb();
    }

    // --- Framework-internal ---

    void set_templates_dir(const std::string& dir) { state_->templates_dir = dir; }

    int status_code() const { return state_->status_code; }
    const std::string& body() const { return state_->body; }

    std::string build() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << state_->status_code << ' ' << reason_phrase(state_->status_code) << "\r\n";
        os << "Content-Length: " << state_->body.size() << "\r\n";
        for (const auto& [k, v] : state_->headers) {
            os << k << ": " << v << "\r\n";
        }
        os << "\r\n" << state_->body;
        return os.str();
    }

private:
    static bool is_template_name(const std::string& s) {
        if (s.empty() || s.find('\n') != std::string::npos) return false;
        if (s.find('<')  != std::string::npos) return false;
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
