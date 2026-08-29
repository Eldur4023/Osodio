#pragma once
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace odio {

// Valor en tiempo de ejecucion del VM.
//
// Ocupa 16 bytes: 8 de carga y 1 de etiqueta.  Un valor es exactamente de un
// tipo a la vez, asi que el puntero al monton comparte hueco con los escalares
// en una union.  Antes eran 72 —tres shared_ptr, dos de ellos siempre nulos—, y
// cada push y cada pop del VM movia esos 72 bytes.
class Value {
public:
    enum class Type { Null, Bool, Int, Float, Str, List, Dict };

    using List = std::vector<Value>;
    using Dict = std::map<std::string, Value>;

    Value() = default;
    ~Value() { soltar(); }

    Value(const Value& o) : type_(o.type_) { carga_de(o); retener(); }
    Value(Value&& o) noexcept : type_(o.type_) { carga_de(o); o.type_ = Type::Null; }

    Value& operator=(const Value& o) {
        if (this != &o) {
            o.retener();            // antes de soltar: puede ser la misma caja
            soltar();
            type_ = o.type_;
            carga_de(o);
        }
        return *this;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            soltar();
            type_ = o.type_;
            carga_de(o);
            o.type_ = Type::Null;
        }
        return *this;
    }

    static Value null()               { return Value(); }
    static Value boolean(bool b)      { Value v; v.type_ = Type::Bool;  v.b_ = b; return v; }
    static Value integer(long long i) { Value v; v.type_ = Type::Int;   v.i_ = i; return v; }
    static Value real(double d)       { Value v; v.type_ = Type::Float; v.d_ = d; return v; }

    static Value str(std::string s) {
        Value v; v.type_ = Type::Str;  v.o_ = new CStr(std::move(s));  return v;
    }
    static Value list(List l = {}) {
        Value v; v.type_ = Type::List; v.o_ = new CList(std::move(l)); return v;
    }
    static Value dict(Dict d = {}) {
        Value v; v.type_ = Type::Dict; v.o_ = new CDict(std::move(d)); return v;
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
    const std::string& as_str()   const { return static_cast<CStr*>(o_)->v;  }
    List&              as_list()  const { return static_cast<CList*>(o_)->v; }
    Dict&              as_dict()  const { return static_cast<CDict*>(o_)->v; }

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
            case Type::Str:   return !as_str().empty();
            case Type::List:  return !as_list().empty();
            case Type::Dict:  return !as_dict().empty();
        }
        return false;
    }

    const char* type_name() const;
    std::string to_string() const;          // representacion para text()/concatenacion
    nlohmann::json to_json() const;         // serializacion de respuesta

    static Value from_json(const nlohmann::json& j);

    bool equals(const Value& o) const;

private:
    // Caja con contador propio.  Atomico porque un valor puede nacer en un hilo
    // del pool de base de datos y consumirse en el del event loop: el traspaso
    // esta sincronizado, pero la caja puede quedar compartida entre los dos.
    struct Caja { std::atomic<unsigned> rc{1}; };
    template <typename T>
    struct CajaDe : Caja { T v; explicit CajaDe(T x) : v(std::move(x)) {} };

    using CStr  = CajaDe<std::string>;
    using CList = CajaDe<List>;
    using CDict = CajaDe<Dict>;

    bool en_monton() const {
        return type_ == Type::Str || type_ == Type::List || type_ == Type::Dict;
    }
    void carga_de(const Value& o) { std::memcpy(&i_, &o.i_, sizeof(i_)); }
    void retener() const {
        if (en_monton()) o_->rc.fetch_add(1, std::memory_order_relaxed);
    }
    void soltar() {
        if (!en_monton()) return;
        if (o_->rc.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        switch (type_) {
            case Type::Str:  delete static_cast<CStr*>(o_);  break;
            case Type::List: delete static_cast<CList*>(o_); break;
            case Type::Dict: delete static_cast<CDict*>(o_); break;
            default: break;
        }
    }

    union {
        long long i_ = 0;
        bool      b_;
        double    d_;
        Caja*     o_;
    };
    Type type_ = Type::Null;
};

} // namespace odio
