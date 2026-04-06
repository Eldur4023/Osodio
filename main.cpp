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

// JSON mapping for User (Both directions for automatic serialization)
void from_json(const nlohmann::json& j, User& u) {
    u.name = j.at("name").get<std::string>();
    u.age = j.at("age").get<int>();
}

void to_json(nlohmann::json& j, const User& u) {
    j = nlohmann::json{{"name", u.name}, {"age", u.age}};
}

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
        fprintf(stderr, "[DEBUG] Handler: req.loop address = %p\n", (void*)req.loop);
        co_await sleep(500, req.loop); // Non-blocking sleep
        co_return nlohmann::json{{"status", "async-ok"}, {"waited_ms", 500}};
    });

    app.get("/users/:id", [](PathParam<std::string, "id"> id) -> User {
        return User{id.value, 25};
    });

    app.post("/users", [](Body<User> user) {
        return nlohmann::json{{"id", 1}, {"name", user->name}};
    });

    std::cout << "Starting Osodio on port 8080..." << std::endl;
    app.run(8080);

    return 0;
}
