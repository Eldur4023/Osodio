#pragma once
#include <string>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"
#include "diagnostic.hpp"

namespace odio {

// Traduce el cuerpo de una ruta a bytecode.
//
// Resuelve los nombres a ranuras locales en tiempo de compilacion, asi que el
// VM nunca busca una variable por nombre: LoadLocal es un indice directo.
class Emitter {
public:
    explicit Emitter(DiagnosticBag& diags) : diags_(diags) {}

    // Los parametros de la ruta ocupan las primeras ranuras, en orden.
    // Devuelve false si algo del cuerpo no se puede compilar todavia.
    bool emit_route(const RouteDecl& route, Chunk& out);

    // Compila una expresion suelta con `names` ya declarados como locales, en
    // ese orden.  Lo usan las reglas de `validate`: cada una se convierte en un
    // chunk diminuto que recibe los campos y devuelve un booleano.
    bool emit_condition(const Expr& e, const std::vector<std::string>& names,
                        Chunk& out);

private:
    DiagnosticBag& diags_;
    Chunk*         chunk_ = nullptr;
    bool           failed_ = false;

    // Metodo de la ruta que se esta compilando: permite rechazar `sse.*` fuera
    // de una ruta sse al compilar, en vez de dejarlo para runtime.
    std::string route_method_;

    struct Local { std::string name; int depth; };
    std::vector<Local> locals_;
    int                scope_depth_ = 0;

    // Saltos pendientes de los break/continue del bucle en curso.
    struct LoopCtx {
        size_t              continue_target;
        std::vector<size_t> breaks;
    };
    std::vector<LoopCtx> loops_;

    void error(SourceLoc loc, std::string msg);

    int  declare_local(const std::string& name, SourceLoc loc);
    int  resolve_local(const std::string& name) const;
    void begin_scope();
    void end_scope();

    void emit_block(const Block& body);
    void emit_stmt(const Stmt& s);
    void emit_expr(const Expr& e);
    void emit_call(const Expr& e, bool awaited);
};

} // namespace odio
