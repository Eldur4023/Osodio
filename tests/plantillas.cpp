// Prueba del motor de plantillas de Odio, sin HTTP de por medio.
#include <odio/plantilla.hpp>

#include <osodio/request.hpp>
#include <osodio/response.hpp>

#include <cstdio>
#include <string>
#include <vector>

using odio::Plantilla;
using odio::Value;

static int fallos = 0;

static void comprobar(const char* nombre, const std::string& obtenido,
                      const std::string& esperado) {
    if (obtenido == esperado) {
        std::printf("  ok    %s\n", nombre);
    } else {
        ++fallos;
        std::printf("  FALLA %s\n    esperado: <<%s>>\n    obtenido: <<%s>>\n",
                    nombre, esperado.c_str(), obtenido.c_str());
    }
}

// Compila y renderiza; devuelve "" y deja `err` si algo falla.
static std::string pintar(const std::string& fuente,
                          const std::vector<std::string>& nombres,
                          std::vector<Value> valores, std::string& err) {
    odio::DiagnosticBag diags;
    Plantilla p;
    if (!odio::compilar_plantilla(fuente, "prueba.html", ".", nombres, diags, p)) {
        err = diags.items().empty() ? "error sin mensaje" : diags.items().front().message;
        return {};
    }
    osodio::Request  req;
    osodio::Response res;
    odio::NativeCtx  ctx{req, res};

    std::string salida;
    if (!odio::render_plantilla(p, std::move(valores), ctx, nullptr, salida, err))
        return {};
    return salida;
}

static void caso(const char* nombre, const std::string& fuente,
                 const std::vector<std::string>& nombres,
                 std::vector<Value> valores, const std::string& esperado) {
    std::string err;
    const std::string got = pintar(fuente, nombres, std::move(valores), err);
    if (!err.empty()) {
        ++fallos;
        std::printf("  FALLA %s\n    error inesperado: %s\n", nombre, err.c_str());
        return;
    }
    comprobar(nombre, got, esperado);
}

// Casos que TIENEN que fallar al compilar, con el motivo esperado.
static void no_compila(const char* nombre, const std::string& fuente,
                       const std::vector<std::string>& nombres,
                       const std::string& trozo) {
    std::string err;
    const std::string got = pintar(fuente, nombres, {}, err);
    if (err.empty()) {
        ++fallos;
        std::printf("  FALLA %s\n    compilo cuando no debia: <<%s>>\n", nombre, got.c_str());
        return;
    }
    if (err.find(trozo) == std::string::npos) {
        ++fallos;
        std::printf("  FALLA %s\n    esperaba que el error dijera '%s'\n    dijo: %s\n",
                    nombre, trozo.c_str(), err.c_str());
        return;
    }
    std::printf("  ok    %s\n", nombre);
}

int main() {
    std::printf("== texto y expresiones ==\n");
    caso("texto suelto", "hola mundo", {}, {}, "hola mundo");
    caso("expresion simple", "<p>{{ n }}</p>", {"n"}, {Value::integer(42)},
         "<p>42</p>");
    caso("aritmetica", "{{ n * 2 + 1 }}", {"n"}, {Value::integer(20)}, "41");
    caso("cadena", "hola {{ quien }}", {"quien"}, {Value::str("Ana")}, "hola Ana");
    caso("ternario", "{{ n > 10 ? \"alto\" : \"bajo\" }}", {"n"},
         {Value::integer(20)}, "alto");
    caso("metodo", "{{ s.upper() }}", {"s"}, {Value::str("ana")}, "ANA");

    std::printf("== autoescapado ==\n");
    caso("escapa por defecto", "{{ s }}", {"s"},
         {Value::str("<script>alert('x')</script>")},
         "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;");
    caso("marcador safe", "{{ s|safe }}", {"s"}, {Value::str("<b>ok</b>")},
         "<b>ok</b>");
    caso("ampersand y comillas", "{{ s }}", {"s"}, {Value::str("a & \"b\"")},
         "a &amp; &quot;b&quot;");

    std::printf("== miembros y campos ==\n");
    {
        Value::Dict d;
        d["titulo"] = Value::str("Hola");
        d["vistas"] = Value::integer(7);
        caso("campo de dict", "{{ a.titulo }} ({{ a.vistas }})", {"a"},
             {Value::dict(std::move(d))}, "Hola (7)");
    }

    std::printf("== condicionales ==\n");
    caso("if verdadero", "{% if n > 5 %}grande{% endif %}", {"n"},
         {Value::integer(9)}, "grande");
    caso("if falso", "{% if n > 5 %}grande{% endif %}", {"n"},
         {Value::integer(1)}, "");
    caso("if else", "{% if n > 5 %}grande{% else %}pequeno{% endif %}", {"n"},
         {Value::integer(1)}, "pequeno");
    caso("elif", "{% if n > 100 %}enorme{% elif n > 5 %}grande{% else %}pequeno{% endif %}",
         {"n"}, {Value::integer(9)}, "grande");
    caso("elif ultimo", "{% if n > 100 %}enorme{% elif n > 5 %}grande{% else %}pequeno{% endif %}",
         {"n"}, {Value::integer(1)}, "pequeno");
    caso("veracidad de odio", "{% if s %}hay{% else %}vacio{% endif %}", {"s"},
         {Value::str("")}, "vacio");

    std::printf("== bucles ==\n");
    {
        Value::List l;
        l.push_back(Value::str("a"));
        l.push_back(Value::str("b"));
        l.push_back(Value::str("c"));
        caso("for simple", "{% for x in xs %}[{{ x }}]{% endfor %}", {"xs"},
             {Value::list(l)}, "[a][b][c]");
        caso("for con loop.index", "{% for x in xs %}{{ loop.index }}:{{ x }} {% endfor %}",
             {"xs"}, {Value::list(l)}, "1:a 2:b 3:c ");
        caso("loop.first y last", "{% for x in xs %}{% if loop.first %}<{% endif %}{{ x }}{% if loop.last %}>{% endif %}{% endfor %}",
             {"xs"}, {Value::list(l)}, "<abc>");
        caso("for vacio", "[{% for x in xs %}{{ x }}{% endfor %}]", {"xs"},
             {Value::list({})}, "[]");
        caso("if dentro de for", "{% for x in xs %}{% if x != \"b\" %}{{ x }}{% endif %}{% endfor %}",
             {"xs"}, {Value::list(l)}, "ac");
    }
    {
        // Anidado, con sombreado del mismo nombre.
        Value::List interna;
        interna.push_back(Value::integer(1));
        interna.push_back(Value::integer(2));
        Value::List externa;
        externa.push_back(Value::list(interna));
        externa.push_back(Value::list(interna));
        caso("bucles anidados", "{% for x in xs %}({% for x in x %}{{ x }}{% endfor %}){% endfor %}",
             {"xs"}, {Value::list(std::move(externa))}, "(12)(12)");
    }

    std::printf("== comentarios y espacios ==\n");
    caso("comentario", "a{# esto no sale #}b", {}, {}, "ab");
    caso("recorte", "a   {%- if true -%}   b{% endif %}", {}, {}, "ab");

    std::printf("== errores de compilacion ==\n");
    no_compila("campo inexistente en la sintaxis", "{{ }}", {}, "expresion");
    no_compila("cierre que falta", "{{ n ", {"n"}, "falta el cierre");
    no_compila("endif suelto", "{% endif %}", {}, "sin {% if %}");
    no_compila("if sin cerrar", "{% if n %}x", {"n"}, "falta {% endif %}");
    no_compila("for sin cerrar", "{% for x in xs %}", {"xs"}, "falta {% endfor %}");
    no_compila("for mal escrito", "{% for x xs %}{% endfor %}", {"xs"}, "for x in lista");
    no_compila("etiqueta desconocida", "{% cosa %}", {}, "etiqueta desconocida");
    no_compila("filtro de jinja", "{{ x|upper }}", {"x"}, "metodos de Odio");
    no_compila("herencia todavia no", "{% extends \"base.html\" %}", {}, "todavia no existe");
    no_compila("variable que no existe", "{{ noexiste }}", {}, "en la expresion");
    no_compila("basura detras", "{{ n n }}", {"n"}, "sobra algo");

    std::printf("\n%s\n", fallos ? "HAY FALLOS" : "todas pasan");
    return fallos ? 1 : 0;
}
