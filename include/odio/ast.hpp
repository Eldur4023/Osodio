#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "token.hpp"

namespace odio {

// ─── Tipos ───────────────────────────────────────────────────────────────────

struct TypeRef {
    std::string          name;      // int, string, List, User, ...
    std::vector<TypeRef> args;      // parametros de List<T> / Dict<K,V>
    bool                 optional = false;   // sufijo '?'
    SourceLoc            loc;

    std::string str() const;
};

// ─── Expresiones ─────────────────────────────────────────────────────────────

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class ExprKind {
    StringLit, IntLit, FloatLit, BoolLit, NullLit,
    Ident, This, Member, Index, Call,
    Unary, Binary, Ternary, Await,
    ListLit, DictLit,
};

// Un argumento de llamada: posicional, o con nombre (`error_window="..."`).
struct Arg {
    std::string name;    // vacio si es posicional
    ExprPtr     value;
    SourceLoc   loc;
};

struct DictEntry {
    ExprPtr key;
    ExprPtr value;
};

struct Expr {
    ExprKind  kind;
    SourceLoc loc;

    // Literales
    std::string text;        // StringLit / Ident / Member.name / operador
    long long   int_value  = 0;
    double      float_value = 0;
    bool        bool_value = false;

    // Estructura
    ExprPtr                object;    // Member/Index/Call: receptor
    ExprPtr                lhs, rhs;  // Binary / Ternary(cond=object)
    std::vector<Arg>       args;      // Call
    std::vector<ExprPtr>   items;     // ListLit
    std::vector<DictEntry> entries;   // DictLit
};

// ─── Sentencias ──────────────────────────────────────────────────────────────

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;
using Block   = std::vector<StmtPtr>;

enum class StmtKind {
    Return, ExprStmt, VarDecl, Assign,
    If, While, For, Require, Try, Break, Continue,
};

struct Stmt {
    StmtKind  kind;
    SourceLoc loc;

    ExprPtr value;      // Return / ExprStmt / VarDecl.init / Assign.rhs / cond
    ExprPtr target;     // Assign.lhs / Require.else / For.iterable

    TypeRef     type;   // VarDecl / For
    std::string name;   // VarDecl / For / Try.catch

    Block body;         // If-then / While / For / Try
    Block orelse;       // If-else / Try-catch

    // Cadena else-if: cada eslabon es un If completo dentro de `orelse`.
};

// ─── Declaraciones ───────────────────────────────────────────────────────────

// Un parametro de endpoint o de funcion.
struct Param {
    TypeRef     type;
    std::string name;
    ExprPtr     default_value;   // nulo si no tiene
    SourceLoc   loc;
};

struct RouteDecl {
    std::string              method;    // "GET", "POST", ..., "SSE", "WS"
    std::string              pattern;   // "/users/:id"
    std::vector<Param>       params;
    std::vector<std::string> origins;   // solo ws
    Block                    body;
    SourceLoc                loc;
    SourceLoc                pattern_loc;
};

struct StaticMount {
    std::string url_prefix;
    std::string fs_root;
    bool        spa = false;
    SourceLoc   loc;
};

struct AppDecl {
    std::string              name;
    std::string              version;
    int                      port = 8080;
    std::string              templates_dir = "./templates";
    std::vector<StaticMount> statics;
    bool                     docs = false, health = false, metrics = false;
    SourceLoc                loc;
    bool                     present = false;
};

struct Program {
    std::vector<RouteDecl> routes;
    AppDecl                app;
};

} // namespace odio
