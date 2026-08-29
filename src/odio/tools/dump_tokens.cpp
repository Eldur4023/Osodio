// Herramienta de desarrollo: vuelca los tokens de un .odio.
// Sirve para verificar el lexer sin depender del parser.
#include <odio/lexer.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: dump_tokens <fichero.odio> [--quiet]\n";
        return 2;
    }
    bool quiet = (argc > 2 && std::string(argv[2]) == "--quiet");

    odio::SourceFile src;
    src.path = argv[1];
    {
        std::ifstream f(src.path, std::ios::binary);
        if (!f) { std::cerr << "no se puede abrir: " << src.path << "\n"; return 2; }
        std::ostringstream ss; ss << f.rdbuf();
        src.text = ss.str();
    }

    odio::DiagnosticBag diags;
    odio::Lexer lexer(src, diags);
    auto tokens = lexer.tokenize();

    if (!quiet) {
        int depth = 0;
        for (const auto& t : tokens) {
            if (t.is(odio::Tok::Dedent)) --depth;
            std::cout << std::string(depth < 0 ? 0 : depth * 2, ' ')
                      << odio::tok_name(t.kind);
            if (!t.text.empty()) std::cout << " \"" << t.text << "\"";
            std::cout << "  @" << t.loc.line << ":" << t.loc.col << "\n";
            if (t.is(odio::Tok::Indent)) ++depth;
        }
    }

    std::cout << "\ntokens: " << tokens.size()
              << "  errores: " << diags.size() << "\n";
    if (!diags.empty()) std::cout << "\n" << diags.format({&src});
    return diags.empty() ? 0 : 1;
}
