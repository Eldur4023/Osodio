#include <osodio/osodio.hpp>
#include <nlohmann/json.hpp>

int main() {
    osodio::App app;

    // app.set_templates("./views");  // opcional, por defecto es ./templates

    app.get("/", [](osodio::Request&, osodio::Response& res) {
        res.html("index.html");   // carga templates/index.html
    });

    app.get("/ping", [](osodio::Request&, osodio::Response& res) {
        res.json({{"status", "ok"}});
    });

    app.get("/users/:id", [](osodio::Request& req, osodio::Response& res) {
        const auto& id = req.params.at("id");
        res.json({{"id", id}, {"name", "Usuario " + id}});
    });

    app.post("/users", [](osodio::Request& req, osodio::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            res.status(201).json({{"created", true}, {"name", body.value("name", "")}});
        } catch (...) {
            res.status(400).json({{"error", "Invalid JSON"}});
        }
    });

    app.run("0.0.0.0", 8080);
}
