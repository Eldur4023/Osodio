#include <odio/natives.hpp>

#include <osodio/request.hpp>
#include <osodio/response.hpp>

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

const std::array<NativeDef, 13> kNatives = {{
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

} // namespace odio
