#include <odio/project.hpp>
#include <odio/lexer.hpp>
#include <odio/parser.hpp>
#include <odio/emitter.hpp>
#include <odio/vm.hpp>
#include <odio/crypto.hpp>

#include <osodio/request.hpp>
#include <osodio/response.hpp>
#include <osodio/task.hpp>
#include <osodio/sse.hpp>
#include <osodio/websocket.hpp>
#include <osodio/multipart.hpp>
#include <osodio/app.hpp>
#include <osodio/logger.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
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

const std::vector<std::string>& scalar_types() {
    static const std::vector<std::string> v =
        {"int", "long", "float", "double", "bool", "string"};
    return v;
}

// El identificador de un builtin asincrono es su indice en la tabla; se
// consulta una vez para no depender del orden.
int async_sleep_id() {
    static const int id = native_id("sleep");
    return id;
}

int async_ws_recv_id() {
    static const int id = native_id("__ws_recv");
    return id;
}

bool is_scalar(const std::string& t) {
    const auto& v = scalar_types();
    return std::find(v.begin(), v.end(), t) != v.end();
}

// ─── Clases ──────────────────────────────────────────────────────────────────

struct ClassField {
    std::string name;
    std::string type;
    bool        optional = false;
};

// Una regla de `validate:` ya compilada: recibe los campos como locales, en el
// orden de declaracion, y devuelve un booleano.
struct ClassRule {
    std::shared_ptr<Chunk> chunk;
    std::string            message;
};

struct ClassInfo {
    std::string             name;
    std::vector<ClassField> fields;
    std::vector<ClassRule>  rules;
};

using ClassTable = std::map<std::string, std::shared_ptr<ClassInfo>>;

void build_classes(const Program& program, const FunctionSigs& fns,
                   const ClassSigs& sigs, ClassTable& out, DiagnosticBag& diags) {
    for (const auto& c : program.classes) {
        if (out.count(c.name)) {
            diags.error(c.loc, "la clase '" + c.name + "' ya esta declarada");
            continue;
        }

        auto info  = std::make_shared<ClassInfo>();
        info->name = c.name;

        std::vector<std::string> field_names;
        bool ok = true;

        for (const auto& f : c.fields) {
            if (!is_scalar(f.type.name)) {
                diags.error(f.loc, "el tipo '" + f.type.str() + "' como campo todavia "
                                   "no esta implementado; de momento solo "
                                   "int, long, float, double, bool y string");
                ok = false;
                continue;
            }
            info->fields.push_back({f.name, f.type.name, f.type.optional});
            field_names.push_back(f.name);
        }
        if (!ok) continue;

        // Cada regla se compila contra los campos de la clase: si menciona un
        // nombre que no existe, el error sale aqui y no en produccion.
        for (const auto& r : c.rules) {
            auto    chunk = std::make_shared<Chunk>();
            Emitter emitter(diags, &fns, &sigs);
            if (!emitter.emit_condition(*r.condition, field_names, *chunk)) continue;
            info->rules.push_back({chunk, r.message});
        }

        out[c.name] = std::move(info);
    }
}

// ─── Sesion firmada y JWT ────────────────────────────────────────────────────
//
// La sesion es una cookie firmada, como en Flask: sin estado en servidor, lo
// que encaja con un VM por peticion y N event loops sin nada que sincronizar.
//
// Formato:  base64url(json) "." base64url(hmac_sha256(secreto, base64url(json)))
//
// El contenido va firmado pero NO cifrado: el usuario puede leerlo, solo no
// puede falsificarlo.  No se guarda ahi nada que no pueda ver.

constexpr const char* kSessionCookie = "osodio_session";

std::string sign_session(const Value::Dict& data, const std::string& secret) {
    std::string payload = crypto::base64url_encode(
        Value::dict(data).to_json().dump());
    std::string mac = crypto::base64url_encode(
        crypto::hmac_sha256(secret, payload));
    return payload + "." + mac;
}

// Devuelve false si la cookie falta, esta mal formada o la firma no cuadra.
// En cualquiera de esos casos la sesion arranca vacia, nunca a medias.
bool load_session(const std::string& cookie, const std::string& secret,
                  Value::Dict& out) {
    size_t dot = cookie.rfind('.');
    if (dot == std::string::npos) return false;

    std::string payload = cookie.substr(0, dot);
    std::string given   = cookie.substr(dot + 1);

    std::string expected = crypto::base64url_encode(
        crypto::hmac_sha256(secret, payload));
    if (!crypto::constant_time_equal(given, expected)) return false;

    std::string json_text;
    if (!crypto::base64url_decode(payload, json_text)) return false;

    auto j = nlohmann::json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    Value v = Value::from_json(j);
    out = v.as_dict();
    return true;
}

// Verifica un JWT HS256 y devuelve los claims.
//
// Comprueba alg, firma y expiracion.  Un token con alg "none", o con RS256
// cuando esperamos HS256, se rechaza: aceptar el alg que diga el token es la
// vulnerabilidad clasica de las librerias de JWT.
bool verify_jwt(const std::string& token, const std::string& secret,
                const std::string& issuer, Value& claims_out) {
    size_t p1 = token.find('.');
    if (p1 == std::string::npos) return false;
    size_t p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string signing_input = token.substr(0, p2);
    std::string given_sig     = token.substr(p2 + 1);

    std::string expected = crypto::base64url_encode(
        crypto::hmac_sha256(secret, signing_input));
    if (!crypto::constant_time_equal(given_sig, expected)) return false;

    std::string header_text, payload_text;
    if (!crypto::base64url_decode(token.substr(0, p1), header_text)) return false;
    if (!crypto::base64url_decode(token.substr(p1 + 1, p2 - p1 - 1), payload_text))
        return false;

    auto header = nlohmann::json::parse(header_text, nullptr, false);
    if (header.is_discarded() || !header.is_object()) return false;
    if (header.value("alg", "") != "HS256") return false;

    auto payload = nlohmann::json::parse(payload_text, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) return false;

    if (payload.contains("exp") && payload["exp"].is_number()) {
        long long now = static_cast<long long>(std::time(nullptr));
        if (payload["exp"].get<long long>() < now) return false;
    }
    if (!issuer.empty() && payload.value("iss", issuer) != issuer) return false;

    claims_out = Value::from_json(payload);
    return true;
}

// Configuracion de autenticacion que cada handler necesita en runtime.
struct AuthConfig {
    std::string session_secret;
    int         session_max_age = 86400;
    bool        session_secure  = true;
    std::string jwt_secret;
    std::string jwt_issuer;
};

// Prepara sesion y claims antes de ejecutar el handler.
void begin_auth(const AuthConfig& cfg, osodio::Request& req,
                SessionState& session, Value& claims, NativeCtx& ctx) {
    session.secret = cfg.session_secret;
    if (!cfg.session_secret.empty()) {
        auto cookie = req.cookie(kSessionCookie);
        if (cookie) load_session(*cookie, cfg.session_secret, session.data);
        session.loaded = true;
    }
    ctx.session = &session;

    if (!cfg.jwt_secret.empty()) {
        auto auth = req.header("authorization");
        if (auth && auth->rfind("Bearer ", 0) == 0) {
            ctx.jwt_ok = verify_jwt(auth->substr(7), cfg.jwt_secret,
                                    cfg.jwt_issuer, claims);
        }
    }
    ctx.jwt_claims = &claims;
}

// Reescribe la cookie solo si el handler toco la sesion.
void end_auth(const AuthConfig& cfg, const SessionState& session,
              osodio::Response& res) {
    if (!session.dirty || cfg.session_secret.empty()) return;

    osodio::CookieOptions opts;
    opts.path      = "/";
    opts.http_only = true;                 // JS no la puede leer
    opts.secure    = cfg.session_secure;
    opts.same_site = osodio::SameSite::Lax;

    if (session.data.empty()) {
        res.clear_cookie(kSessionCookie, opts);
        return;
    }
    opts.max_age = cfg.session_max_age;
    res.cookie(kSessionCookie, sign_session(session.data, cfg.session_secret), opts);
}

// ─── Enlace de parametros ────────────────────────────────────────────────────

enum class BindKind { Path, Query, Body, File, FileList };

struct ParamBind {
    BindKind    kind = BindKind::Path;
    std::string name;
    std::string type;          // escalar, o el nombre de la clase si es Body
    bool        has_default = false;
    std::string default_text;
    std::shared_ptr<ClassInfo> cls;   // solo Body
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

// Comprueba que un valor JSON encaja con el tipo declarado del campo.
// No hay conversion entre familias: un string en un campo int es un error, no
// un intento de parseo.
bool json_fits(const nlohmann::json& j, const std::string& type, Value& out) {
    if (type == "string") {
        if (!j.is_string()) return false;
        out = Value::str(j.get<std::string>());
        return true;
    }
    if (type == "bool") {
        if (!j.is_boolean()) return false;
        out = Value::boolean(j.get<bool>());
        return true;
    }
    if (type == "int" || type == "long") {
        if (!j.is_number_integer()) return false;
        out = Value::integer(j.get<long long>());
        return true;
    }
    if (type == "float" || type == "double") {
        if (!j.is_number()) return false;
        out = Value::real(j.get<double>());
        return true;
    }
    return false;
}

// Valida los parametros contra el patron y produce el plan de enlace.
bool bind_params(const RouteDecl& r, const ClassTable& classes,
                 std::vector<ParamBind>& out, DiagnosticBag& diags) {
    auto in_pattern = pattern_params(r.pattern);
    bool ok         = true;
    bool seen_body  = false;

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
        bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                       != in_pattern.end();

        // File / List<File> se enlazan a las partes multipart con ese nombre.
        bool is_file      = (p.type.name == "File");
        bool is_file_list = (p.type.name == "List" && p.type.args.size() == 1 &&
                             p.type.args[0].name == "File");
        if (is_file || is_file_list) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' esta en el patron de ruta, "
                                   "asi que no puede ser un fichero subido");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "una ruta " + r.method + " no lleva cuerpo");
                ok = false;
                continue;
            }
            ParamBind fb;
            fb.kind = is_file ? BindKind::File : BindKind::FileList;
            fb.name = p.name;
            fb.type = p.type.name;
            out.push_back(std::move(fb));
            continue;
        }

        // Un parametro cuyo tipo es una clase se enlaza al cuerpo de la
        // peticion: es la idea de FastAPI, el body es un parametro tipado mas.
        auto it = classes.find(p.type.name);
        if (it != classes.end()) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' esta en el patron de ruta, "
                                   "asi que no puede ser del tipo '" + p.type.name + "'");
                ok = false;
                continue;
            }
            if (seen_body) {
                diags.error(p.loc, "solo puede haber un parametro de cuerpo por ruta");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "una ruta " + r.method + " no lleva cuerpo");
                ok = false;
                continue;
            }
            seen_body = true;
            out.push_back({BindKind::Body, p.name, p.type.name, false, {}, it->second});
            continue;
        }

        ParamBind b;
        b.name = p.name;
        b.type = p.type.name;

        if (!is_scalar(p.type.name)) {
            diags.error(p.loc, "tipo desconocido: '" + p.type.str() + "'");
            ok = false;
            continue;
        }
        if (p.type.optional) {
            diags.error(p.loc, "los parametros opcionales todavia no estan implementados");
            ok = false;
            continue;
        }

        b.kind = in_path ? BindKind::Path : BindKind::Query;

        if (in_path && p.default_value) {
            diags.error(p.loc, "un parametro de ruta no puede tener valor por defecto");
            ok = false;
            continue;
        }
        if (!in_path && p.default_value) {
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
    // Una ruta con guardas NUNCA puede tomar la via declarativa: la accion
    // nativa no las ejecuta, asi que se saltaria la proteccion del grupo.
    if (!r.guards.empty())                                       return {};
    if (!r.params.empty())                                       return {};
    if (r.body.size() != 1)                                      return {};
    if (r.body[0]->kind != StmtKind::Return || !r.body[0]->value) return {};

    DiagnosticBag scratch;
    Action a = compile_return(*r.body[0]->value, scratch);
    return scratch.empty() ? a : Action{};
}

// Construye la instancia a partir del cuerpo JSON.
//
// Faltar un campo obligatorio, o traerlo con el tipo equivocado, o incumplir
// una regla de validate, es 422 con la lista completa de motivos: se reportan
// todos de una vez, no el primero.  El handler no llega a ejecutarse.
bool bind_body(const ClassInfo& ci, const FunctionTable* fns,
               osodio::Request& req, osodio::Response& res,
               NativeCtx& ctx, Value& out) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
        res.status(400).json({{"error", "JSON invalido"}});
        return false;
    }
    if (!body.is_object()) {
        res.status(422).json({{"error", "Validacion fallida"},
                              {"mensajes", {"el cuerpo tiene que ser un objeto JSON"}}});
        return false;
    }

    std::vector<std::string> messages;
    Value::Dict              fields;
    std::vector<Value>       ordered;

    for (const auto& f : ci.fields) {
        auto it = body.find(f.name);
        if (it == body.end() || it->is_null()) {
            if (!f.optional) messages.push_back(f.name + ": obligatorio");
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        Value v;
        if (!json_fits(*it, f.type, v)) {
            messages.push_back(f.name + ": se esperaba " + f.type);
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        fields[f.name] = v;
        ordered.push_back(std::move(v));
    }

    // Las reglas solo se ejecutan si los campos estan bien: evaluarlas sobre
    // valores ausentes daria errores de tipo en vez del mensaje util.
    if (messages.empty()) {
        thread_local VM rule_vm;
        for (const auto& rule : ci.rules) {
            VM::Result r = rule_vm.start(*rule.chunk, ordered, ctx, fns);
            if (r.status != VM::Status::Done) {
                osodio::log().error("validate de " + ci.name + ": " + r.error);
                res.status(500).json({{"error", r.error}});
                return false;
            }
            if (!r.value.truthy()) messages.push_back(rule.message);
        }
    }

    if (!messages.empty()) {
        // Se dejan a mano del manejador `on error 422` de esta misma peticion.
        last_validation_messages() = messages;
        res.status(422).json({{"error", "Validacion fallida"}, {"mensajes", messages}});
        return false;
    }

    out = Value::dict(std::move(fields));
    return true;
}

// Rellena las ranuras de los parametros a partir de la peticion.
// Devuelve false, con la respuesta ya escrita, si algun valor no encaja.
// Un File del lenguaje son los metadatos mas el indice de su parte en
// ctx.uploads; los bytes no viajan dentro del Value.
Value make_file_value(const osodio::MultipartPart& part, size_t index) {
    Value::Dict d;
    d["name"]         = Value::str(part.name);
    d["filename"]     = Value::str(part.filename);
    d["content_type"] = Value::str(part.content_type);
    d["size"]         = Value::integer(static_cast<long long>(part.body.size()));
    d["__idx"]        = Value::integer(static_cast<long long>(index));
    return Value::dict(std::move(d));
}

bool prepare_args(const std::vector<ParamBind>& binds, const FunctionTable* fns,
                  osodio::Request& req, osodio::Response& res,
                  NativeCtx& ctx, std::vector<Value>& out) {
    // Se limpian al empezar: un 422 que el handler escriba a mano no debe
    // heredar los mensajes de una validacion anterior en este hilo.
    last_validation_messages().clear();
    out.reserve(binds.size());
    for (const auto& b : binds) {
        if (b.kind == BindKind::File || b.kind == BindKind::FileList) {
            if (!ctx.uploads) {
                res.status(400).json({{"error", "se esperaba multipart/form-data"}});
                return false;
            }
            Value::List matches;
            for (size_t i = 0; i < ctx.parts->size(); ++i) {
                const auto& part = (*ctx.parts)[i];
                if (part.name == b.name && !part.filename.empty())
                    matches.push_back(make_file_value(part, i));
            }

            if (b.kind == BindKind::FileList) {
                out.push_back(Value::list(std::move(matches)));
                continue;
            }
            if (matches.empty()) {
                res.status(422).json({{"error", "Validacion fallida"},
                                      {"mensajes", {b.name + ": falta el fichero"}}});
                return false;
            }
            out.push_back(matches[0]);
            continue;
        }

        if (b.kind == BindKind::Body) {
            Value v;
            if (!bind_body(*b.cls, fns, req, res, ctx, v)) return false;
            out.push_back(std::move(v));
            continue;
        }

        std::string raw;
        bool present = false;

        if (b.kind == BindKind::Path) {
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
                {"error",    "parametro invalido"},
                {"param",    b.name},
                {"esperado", b.type},
                {"recibido", raw},
            });
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

// ─── OpenAPI desde el AST ────────────────────────────────────────────────────
//
// La version C++ de esto eran 347 lineas de metaprogramacion que deducian el
// esquema construyendo un T{} por defecto e inspeccionando el JSON resultante.
// Con un AST delante, los nombres y los tipos ya estan ahi: es recorrer
// declaraciones.

std::string openapi_type(const std::string& t) {
    if (t == "int" || t == "long")        return "integer";
    if (t == "float" || t == "double")    return "number";
    if (t == "bool")                      return "boolean";
    return "string";
}

// El patron de Odio usa :nombre; OpenAPI usa {nombre}.
std::string openapi_path(const std::string& pattern) {
    std::string out;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':') {
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != '/') ++j;
            out += "{" + pattern.substr(i + 1, j - i - 1) + "}";
            i = j;
        } else {
            out += pattern[i++];
        }
    }
    return out;
}

nlohmann::json build_openapi(const Program& program, const ClassTable& classes) {
    nlohmann::json doc;
    doc["openapi"] = "3.0.3";
    doc["info"]    = {
        {"title",   program.app.name.empty() ? "Osodio API" : program.app.name},
        {"version", program.app.version.empty() ? "0.1.0" : program.app.version},
    };

    // Las clases se publican como esquemas reutilizables.  Los mensajes de
    // `validate:` se adjuntan como descripcion: son las reglas reales que
    // aplica el servidor, asi que documentan mejor que cualquier texto aparte.
    nlohmann::json schemas = nlohmann::json::object();
    for (const auto& [name, info] : classes) {
        nlohmann::json props    = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();

        for (const auto& f : info->fields) {
            props[f.name] = {{"type", openapi_type(f.type)}};
            if (!f.optional) required.push_back(f.name);
        }

        nlohmann::json schema = {{"type", "object"}, {"properties", props}};
        if (!required.empty()) schema["required"] = required;

        if (!info->rules.empty()) {
            std::string desc = "Reglas de validacion:";
            for (const auto& r : info->rules) desc += "\n- " + r.message;
            schema["description"] = desc;
        }
        schemas[name] = std::move(schema);
    }
    if (!schemas.empty()) doc["components"]["schemas"] = std::move(schemas);

    nlohmann::json paths = nlohmann::json::object();

    for (const auto& r : program.routes) {
        // Las rutas de flujo no encajan en OpenAPI 3.0: se anuncian como GET
        // con la respuesta que realmente devuelven, sin fingir un esquema.
        std::string method = r.method;
        if (method == "SSE" || method == "WS") method = "GET";
        if (method == "*")                     method = "get";
        for (auto& c : method) c = static_cast<char>(::tolower((unsigned char)c));

        auto in_pattern = pattern_params(r.pattern);
        nlohmann::json params    = nlohmann::json::array();
        std::string    body_class;

        for (const auto& p : r.params) {
            if (classes.count(p.type.name)) { body_class = p.type.name; continue; }
            if (p.type.name == "File" ||
                (p.type.name == "List" && !p.type.args.empty() &&
                 p.type.args[0].name == "File")) {
                body_class = "__multipart";
                continue;
            }

            bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                           != in_pattern.end();
            params.push_back({
                {"name",     p.name},
                {"in",       in_path ? "path" : "query"},
                {"required", in_path},
                {"schema",   {{"type", openapi_type(p.type.name)}}},
            });
        }

        nlohmann::json op;
        if (!params.empty()) op["parameters"] = params;

        if (body_class == "__multipart") {
            op["requestBody"] = {
                {"required", true},
                {"content", {{"multipart/form-data",
                    {{"schema", {{"type", "object"}}}}}}},
            };
        } else if (!body_class.empty()) {
            op["requestBody"] = {
                {"required", true},
                {"content", {{"application/json",
                    {{"schema", {{"$ref", "#/components/schemas/" + body_class}}}}}}},
            };
        }

        nlohmann::json responses;
        if (r.method == "SSE") {
            responses["200"] = {{"description", "Flujo de eventos"},
                                {"content", {{"text/event-stream", nlohmann::json::object()}}}};
            op["summary"] = "Server-Sent Events";
        } else if (r.method == "WS") {
            responses["101"] = {{"description", "Cambio a WebSocket"}};
            op["summary"] = "WebSocket";
        } else {
            responses["200"] = {{"description", "OK"}};
        }
        // Solo se declaran los codigos que el servidor produce de verdad.
        if (!body_class.empty() && body_class != "__multipart")
            responses["422"] = {{"description", "Validacion fallida"}};
        if (!r.guards.empty())
            responses["403"] = {{"description", "Guarda del grupo no superada"}};
        op["responses"] = std::move(responses);

        // Cada codigo de `on error` declarado se anuncia en todas las rutas.
        for (const auto& e : program.errors)
            if (e.code >= 400)
                op["responses"][std::to_string(e.code)] =
                    nlohmann::json{{"description", "Manejador propio"}};

        paths[openapi_path(r.pattern)][method] = std::move(op);
    }

    doc["paths"] = std::move(paths);
    return doc;
}

// Las funciones se compilan antes que rutas y manejadores, y todas ven la
// tabla completa: asi pueden llamarse entre si sin importar el orden en que se
// declararon ni el fichero en que estan.
// Rellena FnSig a partir de una lista de parametros, comprobando que ningun
// obligatorio va detras de uno con valor por defecto.
FnSig make_sig(size_t index, const std::vector<Param>& params, DiagnosticBag& diags) {
    FnSig sig;
    sig.index = index;
    bool seen_default = false;
    for (const auto& p : params) {
        sig.defaults.push_back(p.default_value.get());
        if (p.default_value) seen_default = true;
        else {
            if (seen_default)
                diags.error(p.loc, "un parametro sin valor por defecto no puede ir "
                                   "despues de uno que lo tiene");
            ++sig.required;
        }
    }
    return sig;
}

// Metodos y constructores se compilan como funciones con `this` de primer
// parametro, asi que van a la misma tabla que las funciones sueltas.
ClassSigs build_class_signatures(Module& mod, DiagnosticBag& diags) {
    ClassSigs out;

    for (const auto& c : mod.program.classes) {
        if (out.count(c.name)) continue;      // duplicado ya reportado
        ClassSig sig;
        for (const auto& f : c.fields) sig.fields.push_back(f.name);

        for (const auto& m : c.methods) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.methods[m.name] = make_sig(idx, m.params, diags);
        }
        for (const auto& ct : c.ctors) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.ctors[ct.params.size()] = idx;
        }

        // Sin constructor declarado, se ofrece el de mapeo completo: todos los
        // campos en orden.
        if (sig.ctors.empty() && !c.fields.empty()) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.ctors[c.fields.size()] = idx;
        }
        out[c.name] = std::move(sig);
    }
    return out;
}

void emit_class_bodies(Module& mod, const ClassSigs& classes, const FunctionSigs& fns,
                       DiagnosticBag& diags) {
    for (const auto& c : mod.program.classes) {
        auto it = classes.find(c.name);
        if (it == classes.end()) continue;
        const ClassSig& sig = it->second;

        for (const auto& m : c.methods) {
            auto ms = sig.methods.find(m.name);
            if (ms == sig.methods.end()) continue;
            Emitter emitter(diags, &fns, &classes);
            emitter.emit_method(c.name, m, *mod.functions[ms->second.index]);
        }
        for (const auto& ct : c.ctors) {
            auto cs = sig.ctors.find(ct.params.size());
            if (cs == sig.ctors.end()) continue;
            Emitter emitter(diags, &fns, &classes);
            emitter.emit_ctor(c.name, sig.fields, ct, *mod.functions[cs->second]);
        }

        // El constructor implicito: un CtorDecl sintetico con un parametro por
        // campo, en orden de declaracion.
        if (c.ctors.empty() && !c.fields.empty()) {
            CtorDecl implicito;
            implicito.loc = c.loc;
            for (const auto& f : c.fields) {
                Param p;
                p.loc  = f.loc;
                p.name = f.name;
                p.type = f.type;
                implicito.params.push_back(std::move(p));
            }
            auto cs = sig.ctors.find(c.fields.size());
            if (cs != sig.ctors.end()) {
                Emitter emitter(diags, &fns, &classes);
                emitter.emit_ctor(c.name, sig.fields, implicito,
                                  *mod.functions[cs->second]);
            }
        }
    }
}

FunctionSigs build_functions(Module& mod, DiagnosticBag& diags) {
    FunctionSigs index;

    for (const auto& f : mod.program.functions) {
        if (native_id(f.name) >= 0) {
            diags.error(f.loc, "'" + f.name + "' es un builtin: elige otro nombre");
            continue;
        }
        if (index.count(f.name)) continue;   // el parser ya reporto el duplicado

        index[f.name] = make_sig(mod.functions.size(), f.params, diags);
        mod.functions.push_back(std::make_shared<Chunk>());
    }

    for (const auto& f : mod.program.functions) {
        auto it = index.find(f.name);
        if (it == index.end()) continue;
        Emitter emitter(diags, &index);
        emitter.emit_function(f, *mod.functions[it->second.index]);
    }
    return index;
}

void build_error_handlers(Module& mod, const FunctionSigs& fns,
                          const ClassSigs& sigs, DiagnosticBag& diags) {
    for (const auto& e : mod.program.errors) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs);
        if (!emitter.emit_error_handler(e, *chunk)) continue;
        mod.error_handlers[e.code] = std::move(chunk);
    }
}

void build_routes(Module& mod, const ClassTable& classes, const AuthConfig& auth,
                  const FunctionSigs& fns, const ClassSigs& sigs,
                  DiagnosticBag& diags) {
    // El Module posee la tabla y sobrevive a cualquier peticion en vuelo: el
    // dispatcher mantiene vivo su shared_ptr mientras el handler se ejecuta.
    const FunctionTable* fn_table = &mod.functions;

    for (const auto& r : mod.program.routes) {
        if (!r.origins.empty() && r.method != "WS")
            diags.error(r.loc, "origins() solo es valido en rutas ws");

        // ── Rutas ws ─────────────────────────────────────────────────────────
        // El handshake RFC 6455 lo hace el motor; aqui solo se conduce el VM
        // con la conexion ya establecida.
        if (r.method == "WS") {
            if (r.origins.empty()) {
                diags.error(r.loc, "una ruta ws necesita origins(...): sin lista "
                                   "blanca, cualquier web puede abrir la conexion "
                                   "desde el navegador de tu usuario");
                continue;
            }

            std::vector<ParamBind> ws_binds;
            if (!bind_params(r, classes, ws_binds, diags)) continue;
            bool body_param = false;
            for (const auto& b : ws_binds)
                if (b.kind == BindKind::Body) body_param = true;
            if (body_param) {
                diags.error(r.loc, "una ruta ws no lleva cuerpo");
                continue;
            }

            auto    ws_chunk = std::make_shared<Chunk>();
            Emitter ws_emitter(diags, &fns, &sigs);
            if (!ws_emitter.emit_route(r, *ws_chunk)) continue;

            ++mod.vm_routes;
            std::string ws_where = "WS " + r.pattern;
            osodio::App::WSOptions opts;
            opts.allowed_origins = r.origins;

            mod.router.add_internal("GET", r.pattern,
                osodio::App::make_ws_handler(
                    [ws_chunk, ws_binds, ws_where, auth, fn_table]
                    (osodio::WSConnection conn, osodio::Request& req,
                     osodio::Response& res) -> osodio::Task<void> {
                        NativeCtx    ctx{req, res};
                        SessionState session;
                        Value        claims = Value::dict();
                        begin_auth(auth, req, session, claims, ctx);
                        ctx.ws              = &conn;
                        ctx.response_written = true;   // el upgrade ya respondio

                        std::vector<Value> args;
                        if (!prepare_args(ws_binds, fn_table, req, res, ctx, args)) co_return;

                        VM         vm;
                        VM::Result result = vm.start(*ws_chunk, std::move(args), ctx, fn_table);

                        while (result.status == VM::Status::Suspended) {
                            Value produced = Value::null();

                            if (result.await_id == async_ws_recv_id()) {
                                // Primera suspension que devuelve valor: el
                                // mensaje entra en el VM como resultado del
                                // `await`.  null significa conexion cerrada.
                                auto msg = co_await conn.recv();
                                if (msg && !msg->is_close())
                                    produced = Value::str(msg->data);
                            }
                            else if (result.await_id == async_sleep_id()) {
                                long long ms = result.await_args.empty()
                                             ? 0 : result.await_args[0].as_int();
                                if (ms < 0) ms = 0;
                                co_await osodio::sleep(static_cast<int>(ms));
                                if (req.is_cancelled()) co_return;
                            }

                            result = vm.resume(std::move(produced), ctx);
                        }

                        if (result.status == VM::Status::Error) {
                            std::string at = result.error_loc.file
                                ? *result.error_loc.file + ":" +
                                  std::to_string(result.error_loc.line) + ":" +
                                  std::to_string(result.error_loc.col)
                                : ws_where;
                            osodio::log().error(at + ": " + result.error);
                        }
                    },
                    std::move(opts)));
            continue;
        }

        // ── Rutas sse ────────────────────────────────────────────────────────
        // El flujo se abre antes de arrancar el VM y se cierra al terminar el
        // handler; no hay respuesta final que escribir.
        if (r.method == "SSE") {
            std::vector<ParamBind> sse_binds;
            if (!bind_params(r, classes, sse_binds, diags)) continue;
            for (const auto& b : sse_binds) {
                if (b.kind == BindKind::Body) {
                    diags.error(r.loc, "una ruta sse no lleva cuerpo");
                    break;
                }
            }

            auto    sse_chunk = std::make_shared<Chunk>();
            Emitter sse_emitter(diags, &fns, &sigs);
            if (!sse_emitter.emit_route(r, *sse_chunk)) continue;

            ++mod.vm_routes;
            std::string sse_where = "SSE " + r.pattern;
            mod.router.add_internal("GET", r.pattern,
                [sse_chunk, sse_binds, sse_where, auth, fn_table](osodio::Request& req,
                                                        osodio::Response& res)
                    -> osodio::Task<void> {
                    NativeCtx    ctx{req, res};
                    SessionState session;
                    Value        claims = Value::dict();
                    begin_auth(auth, req, session, claims, ctx);

                    std::vector<Value> args;
                    if (!prepare_args(sse_binds, fn_table, req, res, ctx, args)) co_return;

                    // make_sse escribe ya las cabeceras del flujo, asi que la
                    // respuesta cuenta como emitida desde este momento.
                    auto writer = osodio::make_sse(res, req);
                    ctx.sse             = &writer;
                    ctx.response_written = true;

                    VM         vm;
                    VM::Result result = vm.start(*sse_chunk, std::move(args), ctx, fn_table);

                    while (result.status == VM::Status::Suspended) {
                        if (result.await_id == async_sleep_id()) {
                            long long ms = result.await_args.empty()
                                         ? 0 : result.await_args[0].as_int();
                            if (ms < 0) ms = 0;
                            co_await osodio::sleep(static_cast<int>(ms));
                            if (req.is_cancelled()) co_return;
                        }
                        result = vm.resume(Value::null(), ctx);
                    }

                    if (result.status == VM::Status::Error) {
                        std::string at = result.error_loc.file
                            ? *result.error_loc.file + ":" +
                              std::to_string(result.error_loc.line) + ":" +
                              std::to_string(result.error_loc.col)
                            : sse_where;
                        osodio::log().error(at + ": " + result.error);
                    }
                });
            continue;
        }

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
        if (!bind_params(r, classes, binds, diags)) continue;

        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs);
        if (!emitter.emit_route(r, *chunk)) continue;

        ++mod.vm_routes;
        bool needs_upload = false;
        for (const auto& b : binds)
            if (b.kind == BindKind::File || b.kind == BindKind::FileList)
                needs_upload = true;

        std::string where = r.method + " " + r.pattern;
        mod.router.add_internal(r.method, r.pattern,
            [chunk, binds, where, auth, needs_upload, fn_table](osodio::Request& req, osodio::Response& res)
                -> osodio::Task<void> {
                NativeCtx    ctx{req, res};
                SessionState session;
                Value        claims = Value::dict();
                begin_auth(auth, req, session, claims, ctx);

                // Solo se parsea el cuerpo multipart si alguna ranura lo pide.
                std::vector<osodio::MultipartPart> parts;
                if (needs_upload) {
                    if (auto p = osodio::parse_multipart(req)) {
                        parts       = std::move(*p);
                        ctx.parts   = &parts;
                        ctx.uploads = true;
                    }
                }

                std::vector<Value> args;
                if (!prepare_args(binds, fn_table, req, res, ctx, args)) co_return;

                // El VM vive en el marco de esta corrutina, no en el hilo: dos
                // handlers suspendidos a la vez sobre el mismo core tienen cada
                // uno su pila y sus locales.  Los que no pueden suspenderse
                // reutilizan uno por hilo y se ahorran las dos reservas.
                thread_local VM shared_vm;
                VM  own_vm;
                VM& vm = chunk->has_await ? own_vm : shared_vm;

                VM::Result result = vm.start(*chunk, std::move(args), ctx, fn_table);

                // El VM no sabe esperar: cada vez que se detiene, el co_await
                // de verdad ocurre aqui, sobre el motor, y se le devuelve el
                // resultado.
                while (result.status == VM::Status::Suspended) {
                    Value produced = Value::null();

                    if (result.await_id == async_sleep_id()) {
                        long long ms = result.await_args.empty()
                                     ? 0 : result.await_args[0].as_int();
                        if (ms < 0) ms = 0;
                        co_await osodio::sleep(static_cast<int>(ms));
                        // sleep() despierta antes si el cliente se desconecta;
                        // en ese caso no tiene sentido seguir ejecutando.
                        if (req.is_cancelled()) co_return;
                    }

                    result = vm.resume(std::move(produced), ctx);
                }

                if (result.status == VM::Status::Error) {
                    std::string at = result.error_loc.file
                        ? *result.error_loc.file + ":" +
                          std::to_string(result.error_loc.line) + ":" +
                          std::to_string(result.error_loc.col)
                        : where;
                    osodio::log().error(at + ": " + result.error);
                    res.status(500).json({{"error", result.error}, {"en", at}});
                    co_return;
                }

                end_auth(auth, session, res);

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

    // Pasada 2: resolver clases y construir la tabla de rutas.  Las clases van
    // primero porque las rutas se enlazan contra ellas; dentro de cada pasada el
    // orden de los ficheros es indiferente.
    if (diags.empty()) {
        // Orden: primero las firmas de todo lo llamable —funciones sueltas,
        // metodos y constructores—, y solo despues los cuerpos.  Asi cualquiera
        // puede llamar a cualquiera sin importar el orden de declaracion.
        auto fns  = build_functions(*mod, diags);
        auto sigs = build_class_signatures(*mod, diags);
        emit_class_bodies(*mod, sigs, fns, diags);

        ClassTable classes;
        build_classes(mod->program, fns, sigs, classes, diags);

        AuthConfig auth;
        auth.session_secret  = mod->program.app.session_secret;
        auth.session_max_age = mod->program.app.session_max_age;
        auth.session_secure  = mod->program.app.session_secure;
        auth.jwt_secret      = mod->program.app.jwt_secret;
        auth.jwt_issuer      = mod->program.app.jwt_issuer;

        if (diags.empty()) build_routes(*mod, classes, auth, fns, sigs, diags);
        if (diags.empty()) build_error_handlers(*mod, fns, sigs, diags);
        if (diags.empty()) mod->openapi = build_openapi(mod->program, classes);
    }

    // Se devuelve siempre: el llamante mira diags.empty() para saber si
    // publicarlo.  Ver la nota en project.hpp sobre la vida de los SourceLoc.
    return mod;
}

} // namespace odio
