#include <odio/value.hpp>
#include <odio/bytecode.hpp>

#include <cmath>
#include <sstream>

namespace odio {

const char* Value::type_name() const {
    switch (type_) {
        case Type::Null:  return "null";
        case Type::Bool:  return "bool";
        case Type::Int:   return "int";
        case Type::Float: return "float";
        case Type::Str:   return "string";
        case Type::List:  return "List";
        case Type::Dict:  return "Dict";
    }
    return "?";
}

std::string Value::to_string() const {
    switch (type_) {
        case Type::Null:  return "null";
        case Type::Bool:  return b_ ? "true" : "false";
        case Type::Int:   return std::to_string(i_);
        case Type::Float: {
            // Sin ceros de relleno: 2.5 y no 2.500000.
            std::ostringstream ss;
            ss << d_;
            return ss.str();
        }
        case Type::Str:   return as_str();
        case Type::List:
        case Type::Dict:  return to_json().dump();
    }
    return {};
}

nlohmann::json Value::to_json() const {
    switch (type_) {
        case Type::Null:  return nullptr;
        case Type::Bool:  return b_;
        case Type::Int:   return i_;
        case Type::Float: return d_;
        case Type::Str:   return as_str();
        case Type::List: {
            auto arr = nlohmann::json::array();
            for (const auto& v : as_list()) arr.push_back(v.to_json());
            return arr;
        }
        case Type::Dict: {
            // Las claves que empiezan por "__" son internas —por ejemplo el
            // indice con el que un File localiza sus bytes— y no salen nunca en
            // la respuesta.
            auto obj = nlohmann::json::object();
            for (const auto& [k, v] : as_dict())
                if (k.rfind("__", 0) != 0) obj[k] = v.to_json();
            return obj;
        }
    }
    return nullptr;
}

Value Value::from_json(const nlohmann::json& j) {
    if (j.is_null())            return Value::null();
    if (j.is_boolean())         return Value::boolean(j.get<bool>());
    if (j.is_number_integer())  return Value::integer(j.get<long long>());
    if (j.is_number_float())    return Value::real(j.get<double>());
    if (j.is_string())          return Value::str(j.get<std::string>());
    if (j.is_array()) {
        List l;
        l.reserve(j.size());
        for (const auto& item : j) l.push_back(from_json(item));
        return Value::list(std::move(l));
    }
    Dict d;
    for (auto it = j.begin(); it != j.end(); ++it) d[it.key()] = from_json(it.value());
    return Value::dict(std::move(d));
}

bool Value::equals(const Value& o) const {
    // int y float se comparan por valor numerico; el resto exige mismo tipo.
    if (is_num() && o.is_num()) {
        if (is_int() && o.is_int()) return i_ == o.i_;
        return as_float() == o.as_float();
    }
    if (type_ != o.type_) return false;

    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return b_ == o.b_;
        case Type::Str:  return as_str() == o.as_str();
        case Type::List: {
            if (as_list().size() != o.as_list().size()) return false;
            for (size_t i = 0; i < as_list().size(); ++i)
                if (!as_list()[i].equals(o.as_list()[i])) return false;
            return true;
        }
        case Type::Dict: {
            if (as_dict().size() != o.as_dict().size()) return false;
            for (const auto& [k, v] : as_dict()) {
                auto it = o.as_dict().find(k);
                if (it == o.as_dict().end() || !v.equals(it->second)) return false;
            }
            return true;
        }
        default: return false;
    }
}

const char* op_name(Op op) {
    switch (op) {
        case Op::Const:            return "CONST";
        case Op::LoadLocal:        return "LOAD_LOCAL";
        case Op::StoreLocal:       return "STORE_LOCAL";
        case Op::Pop:              return "POP";
        case Op::Add:              return "ADD";
        case Op::Sub:              return "SUB";
        case Op::Mul:              return "MUL";
        case Op::Div:              return "DIV";
        case Op::Mod:              return "MOD";
        case Op::Neg:              return "NEG";
        case Op::Eq:               return "EQ";
        case Op::Ne:               return "NE";
        case Op::Lt:               return "LT";
        case Op::Le:               return "LE";
        case Op::Gt:               return "GT";
        case Op::Ge:               return "GE";
        case Op::Not:              return "NOT";
        case Op::AddInt:           return "ADD_INT";
        case Op::SubInt:           return "SUB_INT";
        case Op::MulInt:           return "MUL_INT";
        case Op::LtInt:            return "LT_INT";
        case Op::LeInt:            return "LE_INT";
        case Op::GtInt:            return "GT_INT";
        case Op::GeInt:            return "GE_INT";
        case Op::Jump:             return "JUMP";
        case Op::JumpIfFalse:      return "JUMP_IF_FALSE";
        case Op::JumpIfFalsePeek:  return "JUMP_IF_FALSE_PEEK";
        case Op::JumpIfTruePeek:   return "JUMP_IF_TRUE_PEEK";
        case Op::MakeList:         return "MAKE_LIST";
        case Op::MakeDict:         return "MAKE_DICT";
        case Op::GetIndex:         return "GET_INDEX";
        case Op::SetIndex:         return "SET_INDEX";
        case Op::IterList:         return "ITER_LIST";
        case Op::GetMember:        return "GET_MEMBER";
        case Op::SetMember:        return "SET_MEMBER";
        case Op::CallFunction:     return "CALL_FUNCTION";
        case Op::CallMethod:       return "CALL_METHOD";
        case Op::CallNative:       return "CALL_NATIVE";
        case Op::CallAsync:        return "CALL_ASYNC";
        case Op::Return:           return "RETURN";
        case Op::ReturnNull:       return "RETURN_NULL";
    }
    return "?";
}

} // namespace odio
