#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace odio {

// Valor en tiempo de ejecucion del VM.
//
// Hay un VM por hilo de event loop y ningun valor se comparte entre ellos, asi
// que los tipos de monton usan shared_ptr sin sincronizacion: el contador nunca
// lo tocan dos hilos.
class Value {
public:
    enum class Type { Null, Bool, Int, Float, Str, List, Dict };

    using List = std::vector<Value>;
    using Dict = std::map<std::string, Value>;

    Value() = default;
    static Value null()               { return Value(); }
    static Value boolean(bool b)      { Value v; v.type_ = Type::Bool;  v.b_ = b; return v; }
    static Value integer(long long i) { Value v; v.type_ = Type::Int;   v.i_ = i; return v; }
    static Value real(double d)       { Value v; v.type_ = Type::Float; v.d_ = d; return v; }

    static Value str(std::string s) {
        Value v; v.type_ = Type::Str;
        v.s_ = std::make_shared<std::string>(std::move(s));
        return v;
    }
    static Value list(List l = {}) {
        Value v; v.type_ = Type::List;
        v.l_ = std::make_shared<List>(std::move(l));
        return v;
    }
    static Value dict(Dict d = {}) {
        Value v; v.type_ = Type::Dict;
        v.m_ = std::make_shared<Dict>(std::move(d));
        return v;
    }

    Type type() const { return type_; }

    bool is_null()  const { return type_ == Type::Null; }
    bool is_bool()  const { return type_ == Type::Bool; }
    bool is_int()   const { return type_ == Type::Int; }
    bool is_float() const { return type_ == Type::Float; }
    bool is_num()   const { return is_int() || is_float(); }
    bool is_str()   const { return type_ == Type::Str; }
    bool is_list()  const { return type_ == Type::List; }
    bool is_dict()  const { return type_ == Type::Dict; }

    bool               as_bool()  const { return b_; }
    long long          as_int()   const { return i_; }
    double             as_float() const { return is_float() ? d_ : double(i_); }
    const std::string& as_str()   const { return *s_; }
    List&              as_list()  const { return *l_; }
    Dict&              as_dict()  const { return *m_; }

    // Verdad de Odio, al estilo Python: son falsos null, false, 0, 0.0, la
    // cadena vacia y los contenedores vacios.
    //
    // Esto NO es la coercion de JavaScript que critica el README: aquello es
    // convertir tipos dentro de '==' a espaldas de quien escribe. Aqui no hay
    // conversion, solo una lectura en contexto booleano — la misma que hacen
    // Python y C++, los dos idiomas de los que sale Odio.
    bool truthy() const {
        switch (type_) {
            case Type::Null:  return false;
            case Type::Bool:  return b_;
            case Type::Int:   return i_ != 0;
            case Type::Float: return d_ != 0;
            case Type::Str:   return !s_->empty();
            case Type::List:  return !l_->empty();
            case Type::Dict:  return !m_->empty();
        }
        return false;
    }

    const char* type_name() const;
    std::string to_string() const;          // representacion para text()/concatenacion
    nlohmann::json to_json() const;         // serializacion de respuesta

    static Value from_json(const nlohmann::json& j);

    bool equals(const Value& o) const;

private:
    Type      type_ = Type::Null;
    bool      b_ = false;
    long long i_ = 0;
    double    d_ = 0;
    std::shared_ptr<std::string> s_;
    std::shared_ptr<List>        l_;
    std::shared_ptr<Dict>        m_;
};

} // namespace odio
