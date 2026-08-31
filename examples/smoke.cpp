// Smoke test del motor tras la cirugía de Osodio 2.0.
// Comprueba las tres cosas que el hito 0 tenía que dejar en pie:
// ficheros estáticos, plantillas Jinja2 y una ruta que devuelve JSON.
#include <osodio/osodio.hpp>

using namespace osodio;

int main() {
    App app;
    app.use(osodio::logger());
    app.set_templates("./templates");

    app.get("/json", [](Response& res) {
        res.json_text(R"({"ok":true,"engine":"osodio-2.0"})");
    });

    // Las plantillas ya no se renderizan desde C++: viven en Odio, se compilan
    // al arrancar y se piden con render() desde un .odio.  Ver ejemplo/.

    app.serve_static("/static", "./public");

    app.run(8080);
}
