#include <odio/project.hpp>
#include <odio/lexer.hpp>
#include <odio/parser.hpp>
#include <odio/emitter.hpp>
#include <odio/vm.hpp>

#include <osodio/request.hpp>
#include <osodio/response.hpp>
#include <osodio/task.hpp>
#include <osodio/logger.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace odio {

// ─── Entrada ─────────────────────────────────────────────────────────────────

bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<fs::path>& out,
                    std::string& error) {
    std::error_code ec;

    for (const auto& a : args) {
        fs::path p(a);
        if (!fs::exists(p, ec)) {
            error = "no existe: " + a;
            return false;
        }

        if (fs::is_directory(p, ec)) {
            size_t before = out.size();
            for (auto it = fs::recursive_directory_iterator(p, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_regular_file(ec) && it->path().extension() == ".odio")
                    out.push_back(it->path());
            }
            if (out.size() == before) {
                error = "el directorio no contiene ningun .odio: " + a;
                return false;
            }
        } else {
            out.push_back(p);
        }
    }

    // Orden estable para que los diagnosticos salgan siempre igual.  El orden
    // no afecta al significado: la resolucion de nombres es en dos pasadas.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    if (out.empty()) { error = "no hay ficheros que compilar"; return false; }
    return true;
}

std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files) {
    std::vector<const SourceFile*> raw;
    raw.reserve(files.size());
    for (const auto& f : files) raw.push_back(f.get());
    return diags.format(raw);
}

// ─── Evaluacion de literales ─────────────────────────────────────────────────
//
// Una ruta declarativa solo puede contener valores constantes: si aparece algo
// que hay que calcular, la ruta necesita el VM y eso es otro hito.

namespace {

bool const_eval(const Expr& e, nlohmann::json& out) {
    switch (e.kind) {
        case ExprKind::StringLit: out = e.text;        return true;
        case ExprKind::IntLit:    out = e.int_value;   return true;
        case ExprKind::FloatLit:  out = e.float_value; return true;
        case ExprKind::BoolLit:   out = e.bool_value;  return true;
        case ExprKind::NullLit:   out = nullptr;       return true;

        case ExprKind::ListLit: {
            out = nlohmann::json::array();
            for (const auto& item : e.items) {
                nlohmann::json v;
                if (!const_eval(*item, v)) return false;
                out.push_back(std::move(v));
            }
            return true;
        }
        case ExprKind::DictLit: {
            out = nlohmann::json::object();
            for (const auto& entry : e.entries) {
                if (entry.key->kind != ExprKind::StringLit) return false;
                nlohmann::json v;
                if (!const_eval(*entry.value, v)) return false;
                out[entry.key->text] = std::move(v);
            }
            return true;
        }
        default:
            return false;
    }
}

// Nombre de la funcion llamada, si es una llamada directa a un identificador.
const std::string* callee_name(const Expr& e) {
    if (e.kind != ExprKind::Call || !e.object) return nullptr;
    if (e.object->kind != ExprKind::Ident)     return nullptr;
    return &e.object->text;
}

using Action = std::function<void(osodio::Request&, osodio::Response&)>;

// Traduce el `return <expr>` de una ruta declarativa a una accion nativa.
// Devuelve un Action vacio y anota el diagnostico si la expresion necesita
// evaluacion en runtime.
Action compile_return(const Expr& e, DiagnosticBag& diags) {
    // return { ... }  /  return "literal"  → cuerpo JSON
    nlohmann::json literal;
    if (const_eval(e, literal)) {
        return [literal](osodio::Request&, osodio::Response& res) {
            res.json(literal);
        };
    }

    const std::string* fn = callee_name(e);
    if (!fn) {
        diags.error(e.loc, "esta expresion necesita el VM, que todavia no esta "
                           "implementado; en el hito 1 una ruta solo puede "
                           "devolver un valor constante o una llamada nativa "
                           "(render, text, html, json, status, redirect, send_file)");
        return {};
    }

    auto need_string = [&](size_t idx, const char* what) -> const std::string* {
        if (e.args.size() <= idx || e.args[idx].value->kind != ExprKind::StringLit) {
            diags.error(e.loc, std::string(*fn) + "() espera " + what +
                               " como cadena literal");
            return nullptr;
        }
        return &e.args[idx].value->text;
    };

    if (*fn == "render") {
        const std::string* tpl = need_string(0, "el nombre de la plantilla");
        if (!tpl) return {};

        // Los argumentos con nombre son las variables Jinja2 de la plantilla.
        nlohmann::json data = nlohmann::json::object();
        for (size_t i = 1; i < e.args.size(); ++i) {
            const Arg& a = e.args[i];
            if (a.name.empty()) {
                diags.error(a.loc, "tras el nombre de la plantilla, los argumentos "
                                   "de render() van con nombre: clave=valor");
                return {};
            }
            nlohmann::json v;
            if (!const_eval(*a.value, v)) {
                diags.error(a.loc, "valor no constante en render(): requiere el VM");
                return {};
            }
            data[a.name] = std::move(v);
        }
        std::string name = *tpl;
        return [name, data](osodio::Request&, osodio::Response& res) {
            res.render(name, data);
        };
    }

    if (*fn == "text" || *fn == "html") {
        const std::string* s = need_string(0, "el contenido");
        if (!s) return {};
        std::string body = *s;
        bool is_html = (*fn == "html");
        return [body, is_html](osodio::Request&, osodio::Response& res) {
            if (is_html) res.html(body); else res.text(body);
        };
    }

    if (*fn == "send_file") {
        const std::string* p = need_string(0, "la ruta del fichero");
        if (!p) return {};
        std::string path = *p;
        return [path](osodio::Request&, osodio::Response& res) {
            res.send_file(path);
        };
    }

    if (*fn == "status") {
        if (e.args.empty() || e.args[0].value->kind != ExprKind::IntLit) {
            diags.error(e.loc, "status() espera un codigo numerico");
            return {};
        }
        int code = static_cast<int>(e.args[0].value->int_value);
        return [code](osodio::Request&, osodio::Response& res) {
            res.status(code).send("");
        };
    }

    if (*fn == "redirect") {
        const std::string* target = need_string(0, "el destino");
        if (!target) return {};
        int code = 302;
        if (e.args.size() > 1) {
            if (e.args[1].value->kind != ExprKind::IntLit) {
                diags.error(e.loc, "el segundo argumento de redirect() es el codigo");
                return {};
            }
            code = static_cast<int>(e.args[1].value->int_value);
        }
        std::string to = *target;
        return [to, code](osodio::Request&, osodio::Response& res) {
            res.status(code).header("Location", to).send("");
        };
    }

    if (*fn == "json") {
        if (e.args.size() != 1) {
            diags.error(e.loc, "json() espera un unico argumento");
            return {};
        }
        nlohmann::json v;
        if (!const_eval(*e.args[0].value, v)) {
            diags.error(e.loc, "json() con valor no constante: requiere el VM");
            return {};
        }
        return [v](osodio::Request&, osodio::Response& res) { res.json(v); };
    }

    diags.error(e.loc, "funcion nativa desconocida: '" + *fn + "'");
    return {};
}

// Extrae los nombres :param / {param} del patron de ruta.
std::vector<std::string> pattern_params(const std::string& pattern) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':' || pattern[i] == '{') {
            char close = (pattern[i] == '{') ? '}' : '/';
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != close) ++j;
            out.push_back(pattern.substr(i + 1, j - i - 1));
            i = j;
        } else ++i;
    }
    return out;
}

// Como se rellena una ranura local antes de ejecutar el handler.
struct ParamBind {
    std::string name;
    std::string type;          // int, long, float, double, bool, string
    bool        from_path   = false;
    bool        has_default = false;
    std::string default_text;
};

// Convierte el texto crudo de la URL al tipo declarado.  Un valor mal formado
// es un 400: lo mando mal el cliente, no es un fallo del servidor.
bool coerce(const std::string& text, const std::string& type, Value& out) {
    try {
        if (type == "string")                    { out = Value::str(text); return true; }
        if (type == "int" || type == "long")     { out = Value::integer(std::stoll(text)); return true; }
        if (type == "float" || type == "double") { out = Value::real(std::stod(text)); return true; }
        if (type == "bool") {
            if (text == "true"  || text == "1") { out = Value::boolean(true);  return true; }
            if (text == "false" || text == "0") { out = Value::boolean(false); return true; }
            return false;
        }
    } catch (...) { return false; }
    return false;
}

// Valida los parametros contra el patron y produce el plan de enlace.
bool bind_params(const RouteDecl& r, std::vector<ParamBind>& out, DiagnosticBag& diags) {
    static const std::vector<std::string> kScalars =
        {"int", "long", "float", "double", "bool", "string"};

    auto in_pattern = pattern_params(r.pattern);
    bool ok = true;

    for (const auto& seg : in_pattern) {
        bool found = false;
        for (const auto& p : r.params) if (p.name == seg) { found = true; break; }
        if (!found) {
            diags.error(r.pattern_loc,
                        "el patron declara ':" + seg + "' pero ningun parametro lo recoge");
            ok = false;
        }
    }

    for (const auto& p : r.params) {
        ParamBind b;
        b.name = p.name;
        b.type = p.type.name;

        if (std::find(kScalars.begin(), kScalars.end(), p.type.name) == kScalars.end()) {
            diags.error(p.loc, "el tipo '" + p.type.str() + "' como parametro de ruta "
                               "todavia no esta implementado; de momento solo "
                               "int, long, float, double, bool y string");
            ok = false;
            continue;
        }
        if (p.type.optional) {
            diags.error(p.loc, "los parametros opcionales todavia no estan implementados");
            ok = false;
            continue;
        }

        b.from_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                      != in_pattern.end();

        if (b.from_path && p.default_value) {
            diags.error(p.loc, "un parametro de ruta no puede tener valor por defecto");
            ok = false;
            continue;
        }
        if (!b.from_path && p.default_value) {
            const Expr& d = *p.default_value;
            if      (d.kind == ExprKind::StringLit) b.default_text = d.text;
            else if (d.kind == ExprKind::IntLit)    b.default_text = std::to_string(d.int_value);
            else if (d.kind == ExprKind::BoolLit)   b.default_text = d.bool_value ? "true" : "false";
            else {
                diags.error(p.loc, "el valor por defecto tiene que ser una constante");
                ok = false;
                continue;
            }
            b.has_default = true;
        }

        out.push_back(std::move(b));
    }
    return ok;
}

// Reconoce la forma puramente declarativa: un unico `return` que se resuelve
// entero en compilacion.  Estas rutas no ejecutan ni un paso de bytecode.
Action try_declarative(const RouteDecl& r) {
    if (!r.params.empty())                                     return {};
    if (r.body.size() != 1)                                    return {};
    if (r.body[0]->kind != StmtKind::Return || !r.body[0]->value) return {};

    DiagnosticBag scratch;
    Action a = compile_return(*r.body[0]->value, scratch);
    return scratch.empty() ? a : Action{};
}

// Rellena las ranuras de los parametros a partir de la peticion.
// Devuelve false, con la respuesta 400 ya escrita, si algun valor no encaja.
bool prepare_args(const std::vector<ParamBind>& binds,
                  osodio::Request& req, osodio::Response& res,
                  std::vector<Value>& out) {
    out.reserve(binds.size());
    for (const auto& b : binds) {
        std::string raw;
        bool present = false;

        if (b.from_path) {
            auto it = req.params.find(b.name);
            if (it != req.params.end()) { raw = it->second; present = true; }
        } else {
            auto it = req.query.find(b.name);
            if (it != req.query.end())  { raw = it->second; present = true; }
            else if (b.has_default)     { raw = b.default_text; present = true; }
        }

        Value v;
        if (!present) {
            if      (b.type == "string") v = Value::str("");
            else if (b.type == "bool")   v = Value::boolean(false);
            else if (b.type == "float" || b.type == "double") v = Value::real(0);
            else                         v = Value::integer(0);
        } else if (!coerce(raw, b.type, v)) {
            res.status(400).json({
                {"error",     "parametro invalido"},
                {"param",     b.name},
                {"esperado",  b.type},
                {"recibido",  raw},
            });
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

void build_routes(Module& mod, DiagnosticBag& diags) {
    for (const auto& r : mod.program.routes) {
        if (r.method == "SSE" || r.method == "WS") {
            diags.error(r.loc, "las rutas " + r.method + " necesitan await, que "
                               "todavia no esta implementado");
            continue;
        }
        if (!r.origins.empty() && r.method != "WS")
            diags.error(r.loc, "origins() solo es valido en rutas ws");

        // Nivel 1: ruta declarativa → accion nativa, cero bytecode.
        if (Action a = try_declarative(r)) {
            ++mod.declarative_routes;
            mod.router.add_internal(r.method, r.pattern,
                [a](osodio::Request& req, osodio::Response& res) -> osodio::Task<void> {
                    a(req, res);
                    co_return;
                });
            continue;
        }

        // Nivel 2: ruta con logica → bytecode sobre el VM.
        std::vector<ParamBind> binds;
        if (!bind_params(r, binds, diags)) continue;

        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags);
        if (!emitter.emit_route(r, *chunk)) continue;

        ++mod.vm_routes;
        std::string where = r.method + " " + r.pattern;
        mod.router.add_internal(r.method, r.pattern,
            [chunk, binds, where](osodio::Request& req, osodio::Response& res)
                -> osodio::Task<void> {
                std::vector<Value> args;
                if (!prepare_args(binds, req, res, args)) co_return;

                NativeCtx ctx{req, res, false};

                // Un VM por hilo de event loop: ni la pila ni el heap se
                // comparten, asi que no hay nada que sincronizar entre cores.
                thread_local VM vm;
                VM::Result result = vm.run(*chunk, std::move(args), ctx);

                if (!result.ok) {
                    std::string at = result.error_loc.file
                        ? *result.error_loc.file + ":" +
                          std::to_string(result.error_loc.line) + ":" +
                          std::to_string(result.error_loc.col)
                        : where;
                    osodio::log().error(at + ": " + result.error);
                    res.status(500).json({{"error", result.error}, {"en", at}});
                    co_return;
                }

                if (ctx.response_written) co_return;
                if (result.value.is_null()) { res.status(204).send(""); co_return; }
                res.json(result.value.to_json());
            });
    }
}

} // namespace

// ─── Compilacion ─────────────────────────────────────────────────────────────

std::shared_ptr<Module> compile(const std::vector<fs::path>& inputs,
                                DiagnosticBag& diags) {
    auto mod = std::make_shared<Module>();
    std::error_code ec;

    for (const auto& path : inputs) {
        auto src  = std::make_unique<SourceFile>();
        src->path = path.string();

        std::ifstream f(path, std::ios::binary);
        if (!f) {
            diags.error({}, "no se puede leer: " + src->path);
            continue;
        }
        std::ostringstream ss; ss << f.rdbuf();
        src->text = ss.str();

        auto stamp = fs::last_write_time(path, ec);
        if (!ec) mod->stamps.emplace_back(path, stamp);

        mod->files.push_back(std::move(src));
    }

    // Pasada 1: lexar y parsear todos los ficheros sobre el mismo Program.
    for (const auto& src : mod->files) {
        Lexer  lexer(*src, diags);
        Parser parser(lexer.tokenize(), diags);
        parser.parse_into(mod->program);
    }

    // Pasada 2: resolver y construir la tabla de rutas.
    if (diags.empty()) build_routes(*mod, diags);

    // Se devuelve siempre: el llamante mira diags.empty() para saber si
    // publicarlo.  Ver la nota en project.hpp sobre la vida de los SourceLoc.
    return mod;
}

} // namespace odio
