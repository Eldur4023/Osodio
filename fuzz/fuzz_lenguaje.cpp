// Arnes de fuzzing para el frontend de Odio: lexer, parser, checker.
//
// Semillas: todos los .odio del directorio dado (por defecto tests/casos).
// Cada caso mutado se escribe a un fichero temporal y se compila con
// odio::compile(), la MISMA funcion que usa `osodio --check` -- se prueba el
// camino real, no una version resumida de el.
//
//   fuzz_lenguaje [directorio-de-semillas] [iteraciones]

#include "chaos.hpp"
#include <odio/project.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static std::vector<std::string> cargar_semillas(const std::string& dir) {
    std::vector<std::string> out;
    if (!fs::exists(dir)) return out;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".odio") continue;
        std::ifstream f(e.path(), std::ios::binary);
        if (!f) continue;
        out.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return out;
}

int main(int argc, char** argv) {
    std::string dir_semillas = argc > 1 ? argv[1] : "tests/casos";
    int iteraciones = argc > 2 ? std::atoi(argv[2]) : 20000;

    auto semillas = cargar_semillas(dir_semillas);
    if (semillas.empty()) {
        std::fprintf(stderr, "fuzz_lenguaje: sin ficheros .odio en %s\n", dir_semillas.c_str());
        return 1;
    }
    std::fprintf(stderr, "fuzz_lenguaje: %zu semillas de %s, %d iteraciones\n", semillas.size(),
                  dir_semillas.c_str(), iteraciones);

    int fallos = chaos::correr(
        "lenguaje", iteraciones, 2000, semillas, [](const std::string& caso) {
            std::string ruta = "/tmp/fuzz_lenguaje_caso_" + std::to_string(getpid()) + ".odio";
            {
                std::ofstream f(ruta, std::ios::binary);
                f << caso;
            }
            odio::DiagnosticBag diags;
            auto mod = odio::compile({fs::path(ruta)}, diags);
            (void)mod;
            std::remove(ruta.c_str());
        });

    return fallos ? 1 : 0;
}
