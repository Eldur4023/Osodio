#include <osodio/osodio.hpp>

using namespace osodio;

struct User {
    std::string id;
    std::string name;
    int age;
};
OSODIO_SCHEMA(User, id, name, age);
OSODIO_VALIDATE(User, 
    check(name, len_min(3)),
    check(age, min(0), max(150))
);

struct CreateUserBody {
    std::string name;
    int age;
};
OSODIO_SCHEMA(CreateUserBody, name, age);
OSODIO_VALIDATE(CreateUserBody,
    check(name, len_min(3)),
    check(age, min(18), max(99))
);

int main() {
    App app;

    app.get("/", [](Response& res) {
        res.html("index.html");
    });

    app.get("/ping", []() {
        return nlohmann::json{{"status", "ok"}};
    });

    app.get("/users/:id", [](PathParam<std::string, "id"> id) -> User {
        return User{std::string(id), "Usuario " + std::string(id), 25};
    });

    app.post("/users", [](Body<CreateUserBody> body) -> User {
        return User{"new-id", body->name, body->age};
    });

    app.run("0.0.0.0", 8080);
}
