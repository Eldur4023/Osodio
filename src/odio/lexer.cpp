#include <odio/lexer.hpp>
#include <cctype>

namespace odio {

namespace {

bool is_ident_start(unsigned char c) {
    // Los bytes >= 0x80 son continuacion UTF-8: se aceptan para que
    // `contraseña` sea un identificador valido.
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool is_ident_char(unsigned char c) {
    return is_ident_start(c) || std::isdigit(c);
}

} // namespace

char Lexer::peek(size_t ahead) const {
    size_t i = pos_ + ahead;
    return i < file_.text.size() ? file_.text[i] : '\0';
}

char Lexer::advance() {
    char c = file_.text[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else           { ++col_; }
    return c;
}

void Lexer::push(Tok kind, SourceLoc loc, std::string text) {
    out_.push_back(Token{kind, std::move(text), loc});
}

void Lexer::skip_line_comment() {
    while (!eof() && peek() != '\n') advance();
}

// Procesa el principio de una linea logica: mide la indentacion y emite
// Indent/Dedent.  Las lineas en blanco y las de solo comentario se saltan sin
// tocar la pila de indentacion.
void Lexer::handle_line_start() {
    while (!eof()) {
        size_t scan   = pos_;
        int    width  = 0;
        bool   tabs   = false;

        while (scan < file_.text.size()) {
            char c = file_.text[scan];
            if (c == ' ')       { ++width; ++scan; }
            else if (c == '\t') { tabs = true; ++width; ++scan; }
            else break;
        }

        // Linea vacia, solo comentario, o retorno de carro suelto: no cuenta.
        if (scan >= file_.text.size()) { pos_ = scan; col_ += width; return; }
        char c = file_.text[scan];
        if (c == '\n' || c == '\r' || c == '#') {
            while (pos_ < scan) advance();
            if (peek() == '#') skip_line_comment();
            if (peek() == '\r') advance();
            if (peek() == '\n') advance();
            continue;
        }

        if (tabs) {
            diags_.error({&file_.path, line_, 1},
                         "indentacion con tabulador: usa espacios");
        }

        // Una linea que empieza por '.' continua la anterior (encadenado sobre
        // el valor devuelto), asi que no genera Indent/Dedent.  El Newline del
        // salto anterior ya se emitio, porque hasta aqui no se sabia que la
        // linea era una continuacion: se retira.
        if (c == '.' && !std::isdigit(static_cast<unsigned char>(
                            scan + 1 < file_.text.size() ? file_.text[scan + 1] : '\0'))) {
            while (pos_ < scan) advance();
            if (!out_.empty() && out_.back().is(Tok::Newline)) out_.pop_back();
            at_line_start_ = false;
            return;
        }

        while (pos_ < scan) advance();

        SourceLoc loc = here();
        if (width > indents_.back()) {
            indents_.push_back(width);
            push(Tok::Indent, loc);
        } else {
            while (width < indents_.back()) {
                indents_.pop_back();
                push(Tok::Dedent, loc);
            }
            if (width != indents_.back()) {
                diags_.error(loc, "la indentacion no coincide con ningun nivel abierto");
                indents_.push_back(width);
            }
        }
        at_line_start_ = false;
        return;
    }
}

void Lexer::lex_number(SourceLoc loc) {
    std::string text;
    while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();

    bool is_float = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        is_float = true;
        text += advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
    }
    push(is_float ? Tok::Float : Tok::Int, loc, std::move(text));
}

void Lexer::lex_string(SourceLoc loc) {
    advance();  // comilla de apertura
    std::string value;
    while (true) {
        if (eof() || peek() == '\n') {
            diags_.error(loc, "cadena sin cerrar");
            break;
        }
        char c = advance();
        if (c == '"') break;
        if (c != '\\') { value += c; continue; }

        if (eof()) { diags_.error(loc, "cadena sin cerrar"); break; }
        char e = advance();
        switch (e) {
            case 'n':  value += '\n'; break;
            case 't':  value += '\t'; break;
            case 'r':  value += '\r'; break;
            case '0':  value += '\0'; break;
            case '"':  value += '"';  break;
            case '\\': value += '\\'; break;
            default:
                diags_.error({&file_.path, line_, col_ - 1},
                             std::string("escape desconocido: \\") + e);
                value += e;
        }
    }
    push(Tok::String, loc, std::move(value));
}

void Lexer::lex_ident(SourceLoc loc) {
    std::string text;
    while (is_ident_char(static_cast<unsigned char>(peek()))) text += advance();
    Tok kind = keyword_or_ident(text);
    push(kind, loc, std::move(text));
}

void Lexer::lex_token() {
    char c = peek();
    SourceLoc loc = here();

    if (std::isdigit(static_cast<unsigned char>(c))) { lex_number(loc); return; }
    if (c == '"')                                    { lex_string(loc); return; }
    if (is_ident_start(static_cast<unsigned char>(c))){ lex_ident(loc);  return; }

    advance();
    switch (c) {
        case '(': ++bracket_depth_; push(Tok::LParen, loc);   return;
        case ')': if (bracket_depth_) --bracket_depth_; push(Tok::RParen, loc);   return;
        case '[': ++bracket_depth_; push(Tok::LBracket, loc); return;
        case ']': if (bracket_depth_) --bracket_depth_; push(Tok::RBracket, loc); return;
        case '{': ++bracket_depth_; push(Tok::LBrace, loc);   return;
        case '}': if (bracket_depth_) --bracket_depth_; push(Tok::RBrace, loc);   return;

        case ',': push(Tok::Comma, loc);    return;
        case ':': push(Tok::Colon, loc);    return;
        case '.': push(Tok::Dot, loc);      return;
        case '?': push(Tok::Question, loc); return;

        case '+':
            if (peek() == '+')      { advance(); push(Tok::PlusPlus, loc); }
            else if (peek() == '=') { advance(); push(Tok::PlusEq, loc); }
            else                    { push(Tok::Plus, loc); }
            return;
        case '*':
            if (peek() == '=') { advance(); push(Tok::StarEq, loc); }
            else               { push(Tok::Star, loc); }
            return;
        case '/':
            if (peek() == '=') { advance(); push(Tok::SlashEq, loc); }
            else               { push(Tok::Slash, loc); }
            return;
        case '%':
            if (peek() == '=') { advance(); push(Tok::PercentEq, loc); }
            else               { push(Tok::Percent, loc); }
            return;

        case '-':
            // '->' del montaje estatico, '--' y '-=' antes que el menos suelto.
            if (peek() == '>')      { advance(); push(Tok::Arrow, loc); }
            else if (peek() == '-') { advance(); push(Tok::MinusMinus, loc); }
            else if (peek() == '=') { advance(); push(Tok::MinusEq, loc); }
            else                    { push(Tok::Minus, loc); }
            return;
        case '=':
            if (peek() == '=') { advance(); push(Tok::Eq, loc); }
            else               { push(Tok::Assign, loc); }
            return;
        case '!':
            if (peek() == '=') { advance(); push(Tok::NotEq, loc); return; }
            diags_.error(loc, "'!' suelto: la negacion se escribe 'not'");
            return;
        case '<':
            if (peek() == '=') { advance(); push(Tok::LtEq, loc); }
            else               { push(Tok::Lt, loc); }
            return;
        case '>':
            // '>' '>' nunca se fusionan: List<Dict<string,int>> tiene que
            // cerrar con dos tokens Gt independientes.
            if (peek() == '=') { advance(); push(Tok::GtEq, loc); }
            else               { push(Tok::Gt, loc); }
            return;
    }

    diags_.error(loc, std::string("caracter inesperado: '") + c + "'");
}

std::vector<Token> Lexer::tokenize() {
    out_.clear();

    while (true) {
        if (at_line_start_ && bracket_depth_ == 0) handle_line_start();
        if (eof()) break;

        char c = peek();

        if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
        if (c == '#') { skip_line_comment(); continue; }

        if (c == '\n') {
            // La posicion se toma ANTES de consumir el salto: un error del tipo
            // "se esperaba ':'" tiene que senalar el final de la linea que lo
            // provoca, no la columna 1 de la siguiente.
            SourceLoc eol = here();
            advance();
            if (bracket_depth_ == 0) {
                // Un Newline solo tiene sentido si la linea produjo tokens.
                if (!out_.empty() && !out_.back().is(Tok::Newline) &&
                    !out_.back().is(Tok::Indent) && !out_.back().is(Tok::Dedent))
                    push(Tok::Newline, eol);
                at_line_start_ = true;
            }
            continue;
        }

        lex_token();
    }

    SourceLoc end{&file_.path, line_, col_};
    if (!out_.empty() && !out_.back().is(Tok::Newline)) push(Tok::Newline, end);
    while (indents_.size() > 1) { indents_.pop_back(); push(Tok::Dedent, end); }
    push(Tok::EndOfFile, end);

    if (bracket_depth_ != 0)
        diags_.error(end, "parentesis o corchete sin cerrar");

    return std::move(out_);
}

} // namespace odio
