#include <odio/natives.hpp>

#include <osodio/request.hpp>
#include <osodio/response.hpp>
#include <osodio/sse.hpp>
#include <osodio/websocket.hpp>

#include <array>

namespace odio {

namespace {

Value fn_text(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.res.text(args[0].to_string());
    ctx.response_written = true;
    return Value::null();
}

Value fn_html(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.res.html(args[0].to_string());
    ctx.response_written = true;
    return Value::null();
}

Value fn_json(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.res.json(args[0].to_json());
    ctx.response_written = true;
    return Value::null();
}

// render("plantilla.html", {clave: valor, ...})
// El emisor recoge los argumentos con nombre en el diccionario del segundo
// hueco, asi que aqui ya llega como un valor mas.
Value fn_render(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) {
        error = "render() espera el nombre de la plantilla como string, no ";
        error += args[0].type_name();
        return Value::null();
    }
    nlohmann::json data = nlohmann::json::object();
    if (args.size() > 1) {
        if (!args[1].is_dict()) {
            error = "las variables de render() tienen que ser un Dict";
            return Value::null();
        }
        data = args[1].to_json();
    }
    ctx.res.render(args[0].as_str(), data);
    ctx.response_written = true;
    return Value::null();
}

Value fn_status(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) {
        error = "status() espera un codigo entero";
        return Value::null();
    }
    ctx.res.status(static_cast<int>(args[0].as_int())).send("");
    ctx.response_written = true;
    return Value::null();
}

Value fn_redirect(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) {
        error = "redirect() espera el destino como string";
        return Value::null();
    }
    int code = 302;
    if (args.size() > 1) {
        if (!args[1].is_int()) {
            error = "el segundo argumento de redirect() es el codigo";
            return Value::null();
        }
        code = static_cast<int>(args[1].as_int());
    }
    ctx.res.status(code).header("Location", args[0].as_str()).send("");
    ctx.response_written = true;
    return Value::null();
}

Value fn_send_file(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) {
        error = "send_file() espera una ruta como string";
        return Value::null();
    }
    ctx.res.send_file(args[0].as_str());
    ctx.response_written = true;
    return Value::null();
}

Value fn_len(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_str())  return Value::integer((long long)v.as_str().size());
    if (v.is_list()) return Value::integer((long long)v.as_list().size());
    if (v.is_dict()) return Value::integer((long long)v.as_dict().size());
    error = std::string("len() no aplica a ") + v.type_name();
    return Value::null();
}

Value fn_str(NativeCtx&, std::vector<Value>& args, std::string&) {
    return Value::str(args[0].to_string());
}

Value fn_int(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_int())   return v;
    if (v.is_float()) return Value::integer((long long)v.as_float());
    if (v.is_bool())  return Value::integer(v.as_bool() ? 1 : 0);
    if (v.is_str()) {
        try { return Value::integer(std::stoll(v.as_str())); }
        catch (...) { error = "int(): '" + v.as_str() + "' no es un numero"; }
        return Value::null();
    }
    error = std::string("int() no aplica a ") + v.type_name();
    return Value::null();
}

Value fn_header(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "header() espera el nombre como string"; return Value::null(); }
    auto h = ctx.req.header(args[0].as_str());
    if (!h) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(*h);
}

Value fn_query(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "query() espera el nombre como string"; return Value::null(); }
    auto it = ctx.req.query.find(args[0].as_str());
    if (it == ctx.req.query.end()) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(it->second);
}

// ─── sse.* ───────────────────────────────────────────────────────────────────
// El objeto `sse` solo existe dentro de una ruta sse; fuera, ctx.sse es nulo y
// el builtin lo dice en vez de reventar.

bool need_sse(NativeCtx& ctx, std::string& error, const char* what) {
    if (ctx.sse) return true;
    error = std::string("'sse.") + what + "' solo existe dentro de una ruta sse";
    return false;
}

// sse.send(datos)
// sse.send(evento, datos)
// sse.send(evento, datos, id)     ← el id permite al navegador reconectar
Value fn_sse_send(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_sse(ctx, error, "send")) return Value::null();

    if (args.size() == 1) return Value::boolean(ctx.sse->send(args[0].to_string()));

    if (!args[0].is_str()) {
        error = "el nombre del evento tiene que ser string";
        return Value::null();
    }
    std::string id = args.size() > 2 ? args[2].to_string() : std::string();
    return Value::boolean(ctx.sse->send_event(args[0].as_str(),
                                              args[1].to_string(), id));
}

Value fn_sse_ping(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_sse(ctx, error, "ping")) return Value::null();
    return Value::boolean(ctx.sse->ping(args.empty() ? "" : args[0].to_string()));
}

Value fn_sse_open(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!need_sse(ctx, error, "open")) return Value::null();
    return Value::boolean(ctx.sse->is_open());
}

// ─── ws.* ────────────────────────────────────────────────────────────────────

bool need_ws(NativeCtx& ctx, std::string& error, const char* what) {
    if (ctx.ws) return true;
    error = std::string("'ws.") + what + "' solo existe dentro de una ruta ws";
    return false;
}

Value fn_ws_send(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_ws(ctx, error, "send")) return Value::null();
    ctx.ws->send(args[0].to_string());
    return Value::null();
}

Value fn_ws_open(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!need_ws(ctx, error, "open")) return Value::null();
    return Value::boolean(ctx.ws->is_open());
}

Value fn_ws_close(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!need_ws(ctx, error, "close")) return Value::null();
    ctx.ws->close();
    return Value::null();
}

// ─── session.* / jwt.* ───────────────────────────────────────────────────────

Value fn_session_get(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "la sesion no esta configurada: falta 'session: secret ...' en app:";
        return Value::null();
    }
    auto it = ctx.session->data.find(args[0].as_str());
    return it == ctx.session->data.end() ? Value::null() : it->second;
}

Value fn_session_set(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "la sesion no esta configurada: falta 'session: secret ...' en app:";
        return Value::null();
    }
    ctx.session->data[args[0].as_str()] = args[1];
    ctx.session->dirty = true;
    return args[1];
}

Value fn_session_clear(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "la sesion no esta configurada: falta 'session: secret ...' en app:";
        return Value::null();
    }
    ctx.session->data.clear();
    ctx.session->dirty = true;
    return Value::null();
}

Value fn_jwt_valid(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::boolean(ctx.jwt_ok);
}

Value fn_jwt_claims(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    if (!ctx.jwt_ok || !ctx.jwt_claims) return Value::dict();
    return *ctx.jwt_claims;
}

const std::array<NativeDef, 26> kNatives = {{
    // Respuesta
    {"text",      1, 1,  fn_text},
    {"html",      1, 1,  fn_html},
    {"json",      1, 1,  fn_json},
    {"render",    1, 2,  fn_render},
    {"status",    1, 1,  fn_status},
    {"redirect",  1, 2,  fn_redirect},
    {"send_file", 1, 1,  fn_send_file},
    // Utilidades
    {"len",       1, 1,  fn_len},
    {"str",       1, 1,  fn_str},
    {"int",       1, 1,  fn_int},
    // Peticion
    {"header",    1, 2,  fn_header},
    {"query",     1, 2,  fn_query},
    // sse.* — no se escriben asi en Odio: se llega por member_native_id().
    {"__sse_send", 1, 3,  fn_sse_send},
    {"__sse_ping", 0, 1,  fn_sse_ping},
    {"__sse_open", 0, 0,  fn_sse_open},
    // ws.* — igual que sse.*, se llega por member_native_id().
    {"__ws_send",  1, 1,  fn_ws_send},
    {"__ws_open",  0, 0,  fn_ws_open},
    {"__ws_close", 0, 0,  fn_ws_close},
    // session.* / jwt.* — se llega por member_native_id() o, en el caso de
    // session, por el acceso a un campo cualquiera.
    {"__session_get",   1, 1,  fn_session_get},
    {"__session_set",   2, 2,  fn_session_set},
    {"__session_clear", 0, 0,  fn_session_clear},
    {"__jwt_valid",     0, 0,  fn_jwt_valid},
    {"__jwt_claims",    0, 0,  fn_jwt_claims},
    // Asincronos: sin fn, los resuelve el driver del handler.
    {"sleep",     1, 1,  nullptr, true},
    {"__ws_recv", 0, 0,  nullptr, true},
    // Marcador final para que native_count() no dependa del orden.
    {nullptr,     0, 0,  nullptr},
}};

} // namespace

int native_id(const std::string& name) {
    for (size_t i = 0; i + 1 < kNatives.size(); ++i)
        if (name == kNatives[i].name) return static_cast<int>(i);
    return -1;
}

const NativeDef& native_at(int id) { return kNatives[static_cast<size_t>(id)]; }

int native_count() { return static_cast<int>(kNatives.size()) - 1; }

namespace {
struct MemberMap { const char* object; const char* member; const char* native; };

const std::array<MemberMap, 10> kMembers = {{
    {"sse", "send",  "__sse_send"},
    {"sse", "ping",  "__sse_ping"},
    {"sse", "open",  "__sse_open"},
    {"ws",  "send",  "__ws_send"},
    {"ws",  "open",  "__ws_open"},
    {"ws",  "close", "__ws_close"},
    {"ws",  "recv",  "__ws_recv"},
    {"session", "clear",  "__session_clear"},
    {"jwt",     "valid",  "__jwt_valid"},
    {"jwt",     "claims", "__jwt_claims"},
}};
} // namespace

int member_native_id(const std::string& object, const std::string& member) {
    for (const auto& m : kMembers)
        if (object == m.object && member == m.member) return native_id(m.native);
    return -1;
}

bool is_reserved_object(const std::string& name) {
    for (const auto& m : kMembers) if (name == m.object) return true;
    return false;
}

} // namespace odio
