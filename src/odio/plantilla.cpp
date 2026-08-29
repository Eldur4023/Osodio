#include <odio/plantilla.hpp>

#include <odio/emitter.hpp>
#include <odio/lexer.hpp>
#include <odio/parser.hpp>
#include <odio/vm.hpp>

#include <memory>
#include <filesystem>
#include <fstream>

namespace odio {

void escapar_html(const std::string& in, std::string& out) {
    // Se copia de golpe lo que no hay que tocar, como en el escapado de JSON:
    // el texto de una pagina casi nunca lleva metacaracteres.
    size_t limpio = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const char* rep = nullptr;
        switch (in[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: continue;
        }
        out.append(in, limpio, i - limpio);
        out += rep;
        limpio = i + 1;
    }
    out.append(in, limpio, in.size() - limpio);
}

namespace {

// ─── Compilador ──────────────────────────────────────────────────────────────

class Compilador {
public:
    Compilador(const std::string& dir, DiagnosticBag& diags, Plantilla& out)
        : dir_(dir), diags_(diags), out_(out) {}

    bool compilar(const std::string& fuente, const std::string& fichero,
                  const std::vector<std::string>& datos) {
        out_.nombres = datos;
        cuerpo(fuente, fichero, 0);
        if (!abiertos_.empty()) {
            error(abiertos_.back().loc, fichero,
                  "falta {% end" + abiertos_.back().que + " %}");
        }
        return !fallo_;
    }

private:
    struct Abierto {
        std::string          que;      // "if" o "for"
        SourceLoc            loc;
        std::vector<size_t>  parches;  // saltos al final del bloque
        size_t               inicio = 0;
        // Nombres que este bucle tapa, para devolverlos al cerrarlo.
        std::vector<std::pair<size_t, std::string>> tapados;
    };

    // Sombrea un nombre ya visible.  Las ranuras no se pueden mover —cada chunk
    // ya compilado indexa por posicion—, asi que el de fuera se renombra a algo
    // que no es un identificador valido: queda inalcanzable mientras dure el
    // bucle, que es exactamente lo que significa sombrear.
    void tapar(const std::string& nombre, std::vector<std::pair<size_t, std::string>>& tapados) {
        for (size_t i = out_.nombres.size(); i-- > 0;) {
            if (out_.nombres[i] == nombre) {
                tapados.emplace_back(i, out_.nombres[i]);
                // El marcador lleva la ranura para que dos nombres tapados no
                // choquen entre si: el emisor tampoco admite eso.
                out_.nombres[i] = " tapado" + std::to_string(i);
                break;
            }
        }
    }

    const std::string&   dir_;
    DiagnosticBag&       diags_;
    Plantilla&           out_;
    bool                 fallo_ = false;
    std::vector<Abierto> abiertos_;
    std::vector<std::string> pila_ficheros_;   // para detectar ciclos de include

    // Los ficheros se guardan vivos porque SourceLoc apunta a su ruta.
    std::vector<std::unique_ptr<SourceFile>> ficheros_;

    void error(SourceLoc loc, const std::string& fichero, std::string msg) {
        fallo_ = true;
        (void)fichero;
        diags_.error(loc, std::move(msg));
    }

    const std::string* guardar_nombre(const std::string& f) {
        for (const auto& sf : ficheros_)
            if (sf->path == f && sf->text.empty()) return &sf->path;
        ficheros_.push_back(std::make_unique<SourceFile>(SourceFile{f, {}}));
        return &ficheros_.back()->path;
    }

    size_t texto(std::string t) {
        if (t.empty()) return SIZE_MAX;
        out_.textos.push_back(std::move(t));
        out_.code.push_back({Plantilla::Op::Texto,
                             static_cast<uint32_t>(out_.textos.size() - 1), 0, 0, 0, {}});
        return out_.code.size() - 1;
    }

    // Compila una expresion de Odio con los nombres visibles ahora mismo.
    // Devuelve su indice en out_.exprs, o SIZE_MAX si no compila.
    size_t expresion(const std::string& texto_expr, SourceLoc loc,
                     const std::string& fichero) {
        // El trozo se lexa como si fuera un fichero suyo.  Sus errores traen
        // la posicion DENTRO del trozo, que no le sirve a nadie: se reporta el
        // sitio de la plantilla y se adjunta el motivo.
        ficheros_.push_back(std::make_unique<SourceFile>(
            SourceFile{fichero + " (expresion)", texto_expr}));
        SourceFile&   sf = *ficheros_.back();
        DiagnosticBag propios;

        Lexer   lx(sf, propios);
        Parser  ps(lx.tokenize(), propios);
        ExprPtr e = ps.parse_single_expression();
        if (!e || !propios.empty()) {
            error(loc, fichero, "en la expresion de la plantilla: " +
                  (propios.empty() ? std::string("expresion invalida")
                                   : propios.items().front().message));
            return SIZE_MAX;
        }

        Chunk   ch;
        Emitter em(propios, fns_, clases_, imports_);
        if (!em.emit_condition(*e, out_.nombres, ch) || !propios.empty()) {
            error(loc, fichero, "en la expresion de la plantilla: " +
                  (propios.empty() ? std::string("no se puede compilar")
                                   : propios.items().front().message));
            return SIZE_MAX;
        }
        out_.exprs.push_back(std::move(ch));
        return out_.exprs.size() - 1;
    }

    void cuerpo(const std::string& src, const std::string& fichero, int profundidad);

public:
    // Contexto de compilacion del modulo, para que las expresiones puedan
    // llamar a funciones del usuario y construir sus clases.
    const FunctionSigs*          fns_     = nullptr;
    const ClassSigs*             clases_  = nullptr;
    const std::set<std::string>* imports_ = nullptr;
};

// Recorta espacios a la izquierda/derecha segun los marcadores {%- y -%}.
void recortar_izq(std::string& t) {
    size_t n = t.size();
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t' ||
                     t[n - 1] == '\n' || t[n - 1] == '\r')) --n;
    t.resize(n);
}
void recortar_der(size_t& i, const std::string& src) {
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' ||
                              src[i] == '\n' || src[i] == '\r')) ++i;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

// El cuerpo se define fuera para poder recursar en {% include %}.
void Compilador::cuerpo(const std::string& src, const std::string& fichero,
                        int profundidad) {
    size_t i = 0, texto_desde = 0;
    int    linea = 1;

    auto loc_en = [&](size_t pos) {
        SourceLoc l;
        l.file = guardar_nombre(fichero);
        int ln = 1, col = 1;
        for (size_t k = 0; k < pos && k < src.size(); ++k) {
            if (src[k] == '\n') { ++ln; col = 1; } else ++col;
        }
        l.line = ln;
        l.col = col;
        return l;
    };
    (void)linea;

    while (i < src.size()) {
        size_t abre = src.find('{', i);
        if (abre == std::string::npos || abre + 1 >= src.size()) break;
        const char tipo = src[abre + 1];
        if (tipo != '{' && tipo != '%' && tipo != '#') { i = abre + 1; continue; }

        const bool recorta_antes = (abre + 2 < src.size() && src[abre + 2] == '-');
        const char* cierre_str = (tipo == '{') ? "}}" : (tipo == '%') ? "%}" : "#}";
        size_t cierre = src.find(cierre_str, abre + 2);
        if (cierre == std::string::npos) {
            error(loc_en(abre), fichero, std::string("falta el cierre de '") +
                  (tipo == '{' ? "{{" : tipo == '%' ? "{%" : "{#") + "'");
            return;
        }
        const bool recorta_despues = (cierre > abre + 2 && src[cierre - 1] == '-');

        std::string previo = src.substr(texto_desde, abre - texto_desde);
        if (recorta_antes) recortar_izq(previo);
        texto(std::move(previo));

        size_t ini_cont = abre + 2 + (recorta_antes ? 1 : 0);
        size_t fin_cont = cierre - (recorta_despues ? 1 : 0);
        std::string contenido = trim(src.substr(ini_cont, fin_cont - ini_cont));
        const SourceLoc loc = loc_en(abre);

        i = cierre + 2;
        if (recorta_despues) recortar_der(i, src);
        texto_desde = i;

        if (tipo == '#') continue;                       // comentario

        if (tipo == '{') {                               // {{ expresion }}
            bool crudo = false;
            if (contenido.size() > 5 &&
                contenido.compare(contenido.size() - 5, 5, "|safe") == 0) {
                crudo = true;
                contenido = trim(contenido.substr(0, contenido.size() - 5));
            }
            if (contenido.find('|') != std::string::npos) {
                error(loc, fichero,
                      "los filtros de Jinja2 no existen aqui: son metodos de Odio. "
                      "En vez de {{ x|upper }}, escribe {{ x.upper() }}");
                continue;
            }
            size_t k = expresion(contenido, loc, fichero);
            if (k == SIZE_MAX) continue;
            out_.code.push_back({crudo ? Plantilla::Op::EscribirCrudo
                                       : Plantilla::Op::Escribir,
                                 static_cast<uint32_t>(k), 0, 0, 0, loc});
            continue;
        }

        // {% etiqueta ... %}
        std::string etiqueta = contenido.substr(0, contenido.find_first_of(" \t"));
        std::string resto    = trim(contenido.substr(etiqueta.size()));

        if (etiqueta == "if") {
            size_t k = expresion(resto, loc, fichero);
            if (k == SIZE_MAX) continue;
            out_.code.push_back({Plantilla::Op::SaltarSiFalso,
                                 static_cast<uint32_t>(k), 0, 0, 0, loc});
            Abierto ab{"if", loc, {}, out_.code.size() - 1};
            abiertos_.push_back(std::move(ab));

        } else if (etiqueta == "elif" || etiqueta == "else") {
            if (abiertos_.empty() || abiertos_.back().que != "if") {
                error(loc, fichero, "{% " + etiqueta + " %} sin {% if %}");
                continue;
            }
            // El bloque anterior salta al final de toda la cadena.
            out_.code.push_back({Plantilla::Op::Saltar, 0, 0, 0, 0, loc});
            abiertos_.back().parches.push_back(out_.code.size() - 1);
            // Y el SaltarSiFalso pendiente aterriza aqui.
            out_.code[abiertos_.back().inicio].b =
                static_cast<uint32_t>(out_.code.size());

            if (etiqueta == "elif") {
                size_t k = expresion(resto, loc, fichero);
                if (k == SIZE_MAX) continue;
                out_.code.push_back({Plantilla::Op::SaltarSiFalso,
                                     static_cast<uint32_t>(k), 0, 0, 0, loc});
                abiertos_.back().inicio = out_.code.size() - 1;
            } else {
                abiertos_.back().inicio = SIZE_MAX;   // no queda condicion pendiente
            }

        } else if (etiqueta == "endif") {
            if (abiertos_.empty() || abiertos_.back().que != "if") {
                error(loc, fichero, "{% endif %} sin {% if %}");
                continue;
            }
            Abierto ab = std::move(abiertos_.back());
            abiertos_.pop_back();
            if (ab.inicio != SIZE_MAX)
                out_.code[ab.inicio].b = static_cast<uint32_t>(out_.code.size());
            for (size_t p : ab.parches)
                out_.code[p].b = static_cast<uint32_t>(out_.code.size());

        } else if (etiqueta == "for") {
            // for <nombre> in <expresion>
            size_t en = resto.find(" in ");
            if (en == std::string::npos) {
                error(loc, fichero, "se esperaba {% for x in lista %}");
                continue;
            }
            std::string var   = trim(resto.substr(0, en));
            std::string lista = trim(resto.substr(en + 4));
            if (var.empty()) {
                error(loc, fichero, "falta el nombre de la variable del bucle");
                continue;
            }
            size_t k = expresion(lista, loc, fichero);
            if (k == SIZE_MAX) continue;

            // Las ranuras solo crecen, y los nombres que este bucle tape se
            // apuntan para devolverlos en el {% endfor %}.
            std::vector<std::pair<size_t, std::string>> tapados;
            tapar(var, tapados);
            tapar("loop", tapados);

            const uint32_t slot = static_cast<uint32_t>(out_.nombres.size());
            out_.nombres.push_back(var);
            const uint32_t slot_loop = static_cast<uint32_t>(out_.nombres.size());
            out_.nombres.push_back("loop");

            out_.code.push_back({Plantilla::Op::BucleInicio,
                                 static_cast<uint32_t>(k), 0, slot, slot_loop, loc});
            Abierto ab{"for", loc, {}, out_.code.size() - 1, std::move(tapados)};
            abiertos_.push_back(std::move(ab));

        } else if (etiqueta == "endfor") {
            if (abiertos_.empty() || abiertos_.back().que != "for") {
                error(loc, fichero, "{% endfor %} sin {% for %}");
                continue;
            }
            Abierto ab = std::move(abiertos_.back());
            abiertos_.pop_back();
            for (const auto& [idx, nom] : ab.tapados) out_.nombres[idx] = nom;
            const auto& ini = out_.code[ab.inicio];
            out_.code.push_back({Plantilla::Op::BucleSiguiente, 0,
                                 static_cast<uint32_t>(ab.inicio + 1),
                                 ini.slot, ini.slot_loop, loc});
            out_.code[ab.inicio].b = static_cast<uint32_t>(out_.code.size());

        } else if (etiqueta == "include") {
            if (profundidad > 16) {
                error(loc, fichero, "demasiados {% include %} anidados: hay un ciclo?");
                continue;
            }
            std::string nombre = trim(resto);
            if (nombre.size() >= 2 && (nombre.front() == '"' || nombre.front() == '\''))
                nombre = nombre.substr(1, nombre.size() - 2);
            std::filesystem::path ruta = std::filesystem::path(dir_) / nombre;
            if (nombre.empty() || nombre.find("..") != std::string::npos) {
                error(loc, fichero, "nombre de plantilla no valido en {% include %}");
                continue;
            }
            std::ifstream f(ruta, std::ios::binary);
            if (!f) {
                error(loc, fichero, "no se encuentra la plantilla '" + nombre + "'");
                continue;
            }
            std::string incluido((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            cuerpo(incluido, nombre, profundidad + 1);

        } else if (etiqueta == "extends" || etiqueta == "block" ||
                   etiqueta == "endblock" || etiqueta == "macro") {
            error(loc, fichero, "{% " + etiqueta + " %} todavia no existe en Odio: "
                                "la herencia de plantillas llegara en su momento");
        } else {
            error(loc, fichero, "etiqueta desconocida: {% " + etiqueta + " %}");
        }
    }

    if (texto_desde < src.size()) texto(src.substr(texto_desde));
}

bool compilar_plantilla(const std::string& fuente, const std::string& fichero,
                        const std::string& dir,
                        const std::vector<std::string>& datos,
                        DiagnosticBag& diags, Plantilla& out) {
    Compilador c(dir, diags, out);
    return c.compilar(fuente, fichero, datos);
}

// ─── Renderizado ─────────────────────────────────────────────────────────────

bool render_plantilla(const Plantilla& p, std::vector<Value> valores,
                      NativeCtx& ctx, const FunctionTable* fns,
                      std::string& salida, std::string& error) {
    // Una ranura por nombre: los datos de entrada ya vienen, el resto son
    // variables de bucle que se van rellenando.
    valores.resize(p.nombres.size());

    // La lista de cada bucle se mantiene viva mientras dura: el item de cada
    // vuelta sale de ella.
    struct Marco { std::shared_ptr<Value> lista; size_t i; };
    std::vector<Marco> bucles;

    thread_local VM vm;

    auto evaluar = [&](uint32_t idx, Value& out) -> bool {
        VM::Result r = vm.start(p.exprs[idx], valores, ctx, fns);
        if (r.status != VM::Status::Done) {
            error = r.error;
            return false;
        }
        out = std::move(r.value);
        return true;
    };

    auto poner_loop = [&](uint32_t slot_loop, size_t i, size_t n) {
        Value::Dict d;
        d.reservar(5);
        d["index"]  = Value::integer(static_cast<long long>(i + 1));
        d["index0"] = Value::integer(static_cast<long long>(i));
        d["first"]  = Value::boolean(i == 0);
        d["last"]   = Value::boolean(i + 1 == n);
        d["length"] = Value::integer(static_cast<long long>(n));
        valores[slot_loop] = Value::dict(std::move(d));
    };

    size_t pc = 0;
    while (pc < p.code.size()) {
        const auto& in = p.code[pc];
        switch (in.op) {
            case Plantilla::Op::Texto:
                salida += p.textos[in.a];
                ++pc;
                break;

            case Plantilla::Op::Escribir: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                escapar_html(v.to_string(), salida);
                ++pc;
                break;
            }
            case Plantilla::Op::EscribirCrudo: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                salida += v.to_string();
                ++pc;
                break;
            }
            case Plantilla::Op::SaltarSiFalso: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                pc = v.truthy() ? pc + 1 : in.b;
                break;
            }
            case Plantilla::Op::Saltar:
                pc = in.b;
                break;

            case Plantilla::Op::BucleInicio: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                if (!v.is_list()) {
                    error = std::string("{% for %} necesita una lista, no ") +
                            v.type_name();
                    return false;
                }
                auto lista = std::make_shared<Value>(std::move(v));
                if (lista->as_list().empty()) { pc = in.b; break; }
                bucles.push_back({lista, 0});
                valores[in.slot] = lista->as_list()[0];
                poner_loop(in.slot_loop, 0, lista->as_list().size());
                ++pc;
                break;
            }
            case Plantilla::Op::BucleSiguiente: {
                Marco& m = bucles.back();
                const auto& l = m.lista->as_list();
                if (++m.i < l.size()) {
                    valores[in.slot] = l[m.i];
                    poner_loop(in.slot_loop, m.i, l.size());
                    pc = in.b;
                } else {
                    bucles.pop_back();
                    ++pc;
                }
                break;
            }
        }
    }
    return true;
}

} // namespace odio
