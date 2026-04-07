#include <osodio/osodio.hpp>
#include <iostream>

using namespace osodio;

struct User {
    std::string name;
    int age;

    // Declarative Validation rules
    OSODIO_VALIDATE(User,
        check(name.size() > 0, "Name cannot be empty"),
        check(age >= 18, "Must be at least 18 years old")
    )
};

OSODIO_SCHEMA(User, name, age)

int main() {
    App app;

    // Middleware: Logging
    app.use([](Request& req, Response& res, auto next) {
        std::cout << "[" << req.method << "] " << req.path << std::endl;
        next();
    });

    app.get("/ping", [](Request& req, Response& res) {
        res.json({{"status", "ok"}});
    });

    // C++20 Coroutine Async Handler
    app.get("/async-ping", [](Request& req) -> Task<nlohmann::json> {
        co_await sleep(500, req.loop);
        co_return nlohmann::json{{"status", "async-ok"}, {"waited_ms", 500}};
    });

    app.get("/users/:id", [](PathParam<std::string, "id"> id) -> User {
        return User{id.value, 25};
    });

    app.post("/users", [](Body<User> user) {
        return nlohmann::json{{"id", 1}, {"name", user->name}};
    });

    // Static files: GET /static/* → ./public/*
    app.serve_static("/static", "./public");

    // Custom error handlers
    app.on_error(404, [](int, Request& req, Response& res) {
        res.json({{"error", "Not Found"}, {"path", req.path}});
    });
    app.on_error([](int code, Request&, Response& res) {
        res.json({{"error", "Something went wrong"}, {"code", code}});
    });

    app.run(8080);

    return 0;
}
