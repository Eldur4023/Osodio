#include <osodio/osodio.hpp>

using namespace osodio;

struct User {
    std::string id;
    std::string name;
};
OSODIO_SCHEMA(User, id, name);

struct CreateUserBody {
    std::string name;
};
OSODIO_SCHEMA(CreateUserBody, name);

int main() {
    App app;

    app.get("/", [](Response& res) {
        res.html("index.html");
    });

    app.get("/ping", []() {
        return nlohmann::json{{"status", "ok"}};
    });

    app.get("/users/:id", [](PathParam<std::string, "id"> id) -> User {
        return User{std::string(id), "Usuario " + std::string(id)};
    });

    app.post("/users", [](Body<CreateUserBody> body) -> User {
        return User{"new-id", body->name};
    });

    app.run("0.0.0.0", 8080);
}
