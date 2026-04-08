#pragma once
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

// ─── HttpError ────────────────────────────────────────────────────────────────
// Throwable from any handler to immediately abort with a typed HTTP response.
// HandlerTraits::call catches this and writes status + body automatically.
//
// Usage:
//   app.get("/users/:id", [](PathParam<int,"id"> id) -> User {
//       auto user = db.find(id.value);
//       if (!user) throw osodio::not_found("User not found");
//       return *user;
//   });

namespace osodio {

struct HttpError : std::exception {
    int            status;
    nlohmann::json body;

    HttpError(int status, nlohmann::json body)
        : status(status), body(std::move(body)) {}

    const char* what() const noexcept override { return "HttpError"; }
};

// ─── Factory helpers ──────────────────────────────────────────────────────────

inline HttpError bad_request(std::string msg = "Bad Request") {
    return {400, {{"error", std::move(msg)}}};
}
inline HttpError unauthorized(std::string msg = "Unauthorized") {
    return {401, {{"error", std::move(msg)}}};
}
inline HttpError forbidden(std::string msg = "Forbidden") {
    return {403, {{"error", std::move(msg)}}};
}
inline HttpError not_found(std::string msg = "Not Found") {
    return {404, {{"error", std::move(msg)}}};
}
inline HttpError method_not_allowed(std::string msg = "Method Not Allowed") {
    return {405, {{"error", std::move(msg)}}};
}
inline HttpError conflict(std::string msg = "Conflict") {
    return {409, {{"error", std::move(msg)}}};
}
inline HttpError unprocessable(std::string msg,
                               nlohmann::json messages = nullptr) {
    nlohmann::json body = {{"error", std::move(msg)}};
    if (!messages.is_null()) body["messages"] = std::move(messages);
    return {422, std::move(body)};
}
inline HttpError too_many_requests(std::string msg = "Too Many Requests") {
    return {429, {{"error", std::move(msg)}}};
}
inline HttpError internal_error(std::string msg = "Internal Server Error") {
    return {500, {{"error", std::move(msg)}}};
}
inline HttpError service_unavailable(std::string msg = "Service Unavailable") {
    return {503, {{"error", std::move(msg)}}};
}

} // namespace osodio
