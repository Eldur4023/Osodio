#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace osodio {

class Response {
public:
    // --- Builder methods ---

    Response& status(int code) {
        status_code_ = code;
        return *this;
    }

    Response& header(std::string key, std::string value) {
        headers_[std::move(key)] = std::move(value);
        return *this;
    }

    Response& text(std::string body) {
        header("Content-Type", "text/plain; charset=utf-8");
        body_ = std::move(body);
        return *this;
    }

    // res.html("index.html")        → carga templates_dir_/index.html
    // res.html("<h1>Hola</h1>")     → HTML inline
    Response& html(const std::string& content) {
        header("Content-Type", "text/html; charset=utf-8");

        if (is_template_name(content)) {
            namespace fs = std::filesystem;
            fs::path path = fs::path(templates_dir_) / content;
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                status_code_ = 500;
                body_ = "Template not found: " + path.string();
                return *this;
            }
            body_ = std::string(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
        } else {
            body_ = content;
        }
        return *this;
    }

    Response& json(const nlohmann::json& j) {
        header("Content-Type", "application/json; charset=utf-8");
        body_ = j.dump();
        return *this;
    }

    Response& send(std::string body) {
        body_ = std::move(body);
        return *this;
    }

    // --- Framework-internal ---

    void set_templates_dir(const std::string& dir) { templates_dir_ = dir; }

    int status_code() const { return status_code_; }
    const std::string& body() const { return body_; }

    std::string build() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << status_code_ << ' ' << reason_phrase(status_code_) << "\r\n";
        os << "Content-Length: " << body_.size() << "\r\n";
        for (const auto& [k, v] : headers_) {
            os << k << ": " << v << "\r\n";
        }
        os << "\r\n" << body_;
        return os.str();
    }

private:
    int         status_code_   = 200;
    std::string body_;
    std::string templates_dir_ = "./templates";
    std::unordered_map<std::string, std::string> headers_;

    // A "template name" is a short string that looks like a filename,
    // not raw HTML content.
    static bool is_template_name(const std::string& s) {
        if (s.empty() || s.find('\n') != std::string::npos) return false;
        if (s.find('<')  != std::string::npos) return false;
        return s.ends_with(".html") || s.ends_with(".htm");
    }

    static const char* reason_phrase(int code) noexcept {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
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
            case 501: return "Not Implemented";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    }
};

} // namespace osodio
