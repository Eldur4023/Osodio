#include <odio/project.hpp>
#include <odio/lexer.hpp>
#include <odio/parser.hpp>

#include <osodio/request.hpp>
#include <osodio/response.hpp>
#include <osodio/task.hpp>

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

// Comprueba que cada :nombre del patron tiene un parametro que lo recoge.
void check_pattern_params(const RouteDecl& r, DiagnosticBag& diags) {
    std::vector<std::string> in_pattern;
    size_t i = 0;
    while (i < r.pattern.size()) {
        if (r.pattern[i] == ':' || r.pattern[i] == '{') {
            char close = (r.pattern[i] == '{') ? '}' : '/';
            size_t j = i + 1;
            while (j < r.pattern.size() && r.pattern[j] != close) ++j;
            in_pattern.push_back(r.pattern.substr(i + 1, j - i - 1));
            i = j;
        } else ++i;
    }

    for (const auto& seg : in_pattern) {
        bool found = false;
        for (const auto& p : r.params) if (p.name == seg) { found = true; break; }
        if (!found)
            diags.error(r.pattern_loc,
                        "el patron declara ':" + seg + "' pero ningun parametro lo recoge");
    }
    for (const auto& p : r.params) {
        bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                       != in_pattern.end();
        if (!in_path && !p.default_value) {
            // Query sin valor por defecto: valido, pero el hito 1 no lo enlaza.
            diags.error(p.loc, "el parametro '" + p.name + "' no aparece en el "
                               "patron; enlazar query y body requiere el VM");
        }
    }
}

void build_routes(Module& mod, DiagnosticBag& diags) {
    for (const auto& r : mod.program.routes) {
        if (r.method == "SSE" || r.method == "WS") {
            diags.error(r.loc, "las rutas " + r.method +
                               " requieren el VM, que todavia no esta implementado");
            continue;
        }
        if (!r.origins.empty() && r.method != "WS")
            diags.error(r.loc, "origins() solo es valido en rutas ws");

        check_pattern_params(r, diags);

        if (r.body.size() != 1 || r.body[0]->kind != StmtKind::Return ||
            !r.body[0]->value) {
            diags.error(r.loc, "en el hito 1 el cuerpo de una ruta tiene que ser "
                               "un unico 'return <valor constante>'");
            continue;
        }

        Action action = compile_return(*r.body[0]->value, diags);
        if (!action) continue;

        mod.router.add_internal(r.method, r.pattern,
            [action](osodio::Request& req, osodio::Response& res) -> osodio::Task<void> {
                action(req, res);
                co_return;
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
