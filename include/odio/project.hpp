#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <osodio/router.hpp>
#include "ast.hpp"
#include "diagnostic.hpp"

namespace odio {

// Un modulo compilado: el resultado de leer un conjunto de .odio.
//
// Es la unidad que se intercambia en caliente.  Recargar es construir un
// Module nuevo y publicar el shared_ptr; si la compilacion falla, el anterior
// sigue en su sitio y no se toca nada.
struct Module {
    std::vector<std::unique_ptr<SourceFile>> files;
    Program        program;
    osodio::Router router;

    // mtimes de los ficheros compilados, para detectar cambios.
    std::vector<std::pair<std::filesystem::path,
                          std::filesystem::file_time_type>> stamps;
};

// Resuelve los argumentos de linea de comandos al conjunto de ficheros a
// compilar:
//   • un fichero  → solo ese
//   • varios      → solo esos
//   • un directorio → todos los .odio que contenga, recursivamente
//
// Devuelve false y escribe en `error` si algun argumento no existe o si un
// directorio no contiene ningun .odio.
bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<std::filesystem::path>& out,
                    std::string& error);

// Lee, lexa, parsea y construye la tabla de rutas.
//
// SIEMPRE devuelve un Module, incluso si hubo errores: los SourceLoc apuntan a
// los SourceFile que este Module posee, asi que destruirlo antes de formatear
// los diagnosticos dejaria punteros colgando.  El exito se comprueba con
// `diags.empty()`, y un modulo con errores simplemente no se publica.
std::shared_ptr<Module> compile(const std::vector<std::filesystem::path>& inputs,
                                DiagnosticBag& diags);

// Formatea los diagnosticos de un intento fallido usando los ficheros leidos.
std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files);

} // namespace odio
