#include <osodio/osodio.hpp>

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
    osodio::App app;

    app.get("/", [](osodio::Request&, osodio::Response& res) {
        res.html("index.html");
    });

    app.get("/ping", [](osodio::Request&, osodio::Response& res) {
        res.json({{"status", "ok"}});
    });

    app.get("/users/:id", [](osodio::Request& req, osodio::Response& res) {
        User u = {req.params.at("id"), "Usuario " + req.params.at("id")};
        res.json(u); // Automatic serialization
    });

    app.post("/users", [](osodio::Request& req, osodio::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body).get<CreateUserBody>();
            res.status(201).json(User{"new-id", body.name});
        } catch (...) {
            res.status(400).json({{"error", "Invalid JSON or schema"}});
        }
    });

    app.run("0.0.0.0", 8080);
}
