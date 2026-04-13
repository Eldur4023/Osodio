#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "app.hpp"
#include "request.hpp"
#include "response.hpp"
#include "cancel.hpp"
#include "task.hpp"

namespace osodio {

// ── TestClient ────────────────────────────────────────────────────────────────
//
// In-process HTTP client for testing Osodio apps without a network socket.
// Exercises the full middleware + router pipeline synchronously.
//
//   App app;
//   app.get("/hello", [](Request&, Response& res) {
//       res.json({{"msg", "hi"}});
//   });
//
//   osodio::TestClient client(app);
//   auto res = client.get("/hello").send();
//   assert(res.status == 200);
//   assert(res.json()["msg"] == "hi");
//
// Notes:
//   - sleep() calls return immediately (req.loop == nullptr → no-op)
//   - SSE and WebSocket handlers are not testable via this client
//   - send_file() responses: the file is read into body so tests can inspect it

class TestClient {
public:
    // ── Response ──────────────────────────────────────────────────────────────

    struct Response {
        int status = 0;
        std::unordered_map<std::string, std::string> headers;
        std::string body;

        // Parse body as JSON. Throws nlohmann::json::parse_error if not valid.
        nlohmann::json json() const { return nlohmann::json::parse(body); }

        // Get a response header by exact name. Returns "" if absent.
        std::string header(const std::string& key) const {
            auto it = headers.find(key);
            return it != headers.end() ? it->second : std::string{};
        }

        // True when status is 2xx.
        bool ok() const noexcept { return status >= 200 && status < 300; }
    };

    // ── RequestBuilder ────────────────────────────────────────────────────────
    //
    // Returned by get() / post() / etc.  Chain builder calls, then call send().
    //
    //   client.post("/users")
    //         .header("Authorization", "Bearer token")
    //         .json({{"name", "Alice"}})
    //         .send();

    class RequestBuilder {
    public:
        // Set a request header.
        RequestBuilder& header(std::string key, std::string val) {
            headers_[std::move(key)] = std::move(val);
            return *this;
        }

        // Set raw body with an explicit Content-Type.
        RequestBuilder& body(std::string b,
                             std::string content_type = "application/octet-stream") {
            body_ = std::move(b);
            if (!content_type.empty())
                headers_["Content-Type"] = std::move(content_type);
            return *this;
        }

        // Serialize `j` as the body and set Content-Type: application/json.
        RequestBuilder& json(const nlohmann::json& j) {
            body_ = j.dump();
            headers_["Content-Type"] = "application/json; charset=utf-8";
            return *this;
        }

        // Append a query parameter (merges with any ?key=val in the path).
        RequestBuilder& query(std::string key, std::string val) {
            extra_query_[std::move(key)] = std::move(val);
            return *this;
        }

        // Execute the request and return the response.
        Response send() {
            return client_.execute(method_, path_, headers_, extra_query_, body_);
        }

    private:
        friend class TestClient;

        TestClient& client_;
        std::string method_;
        std::string path_;
        std::unordered_map<std::string, std::string> headers_;
        std::unordered_map<std::string, std::string> extra_query_;
        std::string body_;

        RequestBuilder(TestClient& c, std::string method, std::string path)
            : client_(c), method_(std::move(method)), path_(std::move(path)) {}
    };

    // ── Construction ──────────────────────────────────────────────────────────

    // Construct a TestClient bound to `app`.
    // Calls app.prepare() to finalize docs routes — safe even if called before
    // app.run() (idempotent).
    explicit TestClient(App& app) : app_(app) { app_.prepare(); }

    // ── HTTP method builders ──────────────────────────────────────────────────

    RequestBuilder get   (std::string path) { return {*this, "GET",    std::move(path)}; }
    RequestBuilder post  (std::string path) { return {*this, "POST",   std::move(path)}; }
    RequestBuilder put   (std::string path) { return {*this, "PUT",    std::move(path)}; }
    RequestBuilder patch (std::string path) { return {*this, "PATCH",  std::move(path)}; }
    RequestBuilder del   (std::string path) { return {*this, "DELETE", std::move(path)}; }

private:
    App& app_;

    // Parse "key=val&key2=val2" into `out` (no URL-decoding — sufficient for tests).
    static void parse_query_string(
        std::string_view s,
        std::unordered_map<std::string, std::string>& out)
    {
        while (!s.empty()) {
            auto amp  = s.find('&');
            auto pair = (amp != std::string_view::npos) ? s.substr(0, amp) : s;
            auto eq   = pair.find('=');
            if (eq != std::string_view::npos)
                out.emplace(std::string(pair.substr(0, eq)),
                            std::string(pair.substr(eq + 1)));
            else if (!pair.empty())
                out.emplace(std::string(pair), std::string{});
            if (amp == std::string_view::npos) break;
            s = s.substr(amp + 1);
        }
    }

    // ── Core execution ────────────────────────────────────────────────────────

    Response execute(const std::string& method,
                     const std::string& raw_path,
                     std::unordered_map<std::string, std::string> headers,
                     const std::unordered_map<std::string, std::string>& extra_query,
                     std::string body_str)
    {
        // ── Build Request ────────────────────────────────────────────────────
        osodio::Request req;
        req.method       = method;
        req.version      = "HTTP/1.1";
        req.body         = std::move(body_str);
        req.headers      = std::move(headers);
        req.cancel_token = std::make_shared<CancellationToken>();
        // loop = nullptr: SleepAwaitable::await_suspend resumes immediately,
        // so co_await sleep(N) becomes a no-op in test context.
        req.loop = nullptr;

        // Split inline query string from path
        auto qpos = raw_path.find('?');
        if (qpos != std::string::npos) {
            req.path = raw_path.substr(0, qpos);
            parse_query_string(raw_path.substr(qpos + 1), req.query);
        } else {
            req.path = raw_path;
        }

        // Merge params added via .query()
        req.query.insert(extra_query.begin(), extra_query.end());

        // ── Run the pipeline synchronously ───────────────────────────────────
        //
        // handle_request() returns Task<void> with initial_suspend=suspend_always.
        // We resume the handle directly.  For standard HTTP handlers the entire
        // middleware + router chain runs on the current stack via C++20 symmetric
        // transfer (each co_await Task calls await_suspend → handle.resume()).
        // No event loop is needed unless the handler calls actual async I/O.

        osodio::Response app_res;
        auto task = app_.handle_request(req, app_res);
        task.handle.resume();

        // Rethrow any unhandled exception (HttpError is caught inside
        // HandlerTraits::call and does NOT reach here under normal use).
        if (task.handle.promise().exception)
            std::rethrow_exception(task.handle.promise().exception);

        if (!task.handle.done())
            throw std::logic_error(
                "osodio::TestClient: handler did not complete synchronously. "
                "SSE and WebSocket handlers cannot be tested via TestClient.");

        // ── Build TestClient::Response ───────────────────────────────────────
        Response result;
        result.status  = app_res.status_code();
        result.headers = app_res.headers_map();

        // sendfile path: read the file into body so tests can inspect content
        if (!app_res.sendfile_path().empty()) {
            std::ifstream f(app_res.sendfile_path(), std::ios::binary);
            result.body = std::string(std::istreambuf_iterator<char>(f),
                                      std::istreambuf_iterator<char>());
        } else {
            result.body = app_res.body();
        }

        return result;
    }
};

} // namespace osodio
