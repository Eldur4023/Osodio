#include <odio/vm.hpp>

namespace odio {

namespace {

// Falla con un mensaje que dice los tipos implicados: un error de ejecucion en
// un .odio tiene que ser tan legible como uno de compilacion.
VM::Result fail(std::string msg, SourceLoc loc) {
    VM::Result r;
    r.status    = VM::Status::Error;
    r.error     = std::move(msg);
    r.error_loc = loc;
    return r;
}

bool numeric_pair(const Value& a, const Value& b) {
    return a.is_num() && b.is_num();
}

// Comparacion de orden: solo entre numeros, o entre cadenas.
bool compare(const Value& a, const Value& b, Op op, bool& ok) {
    ok = true;
    if (numeric_pair(a, b)) {
        if (a.is_int() && b.is_int()) {
            long long x = a.as_int(), y = b.as_int();
            switch (op) {
                case Op::Lt: return x <  y;
                case Op::Le: return x <= y;
                case Op::Gt: return x >  y;
                default:     return x >= y;
            }
        }
        double x = a.as_float(), y = b.as_float();
        switch (op) {
            case Op::Lt: return x <  y;
            case Op::Le: return x <= y;
            case Op::Gt: return x >  y;
            default:     return x >= y;
        }
    }
    if (a.is_str() && b.is_str()) {
        int c = a.as_str().compare(b.as_str());
        switch (op) {
            case Op::Lt: return c <  0;
            case Op::Le: return c <= 0;
            case Op::Gt: return c >  0;
            default:     return c >= 0;
        }
    }
    ok = false;
    return false;
}

} // namespace

VM::Result VM::start(const Chunk& chunk, std::vector<Value> params, NativeCtx& ctx,
                     const FunctionTable* functions) {
    functions_ = functions;
    frames_.clear();
    stack_.clear();
    stack_.reserve(32);

    locals_.assign(static_cast<size_t>(chunk.num_locals), Value::null());
    for (size_t i = 0; i < params.size() && i < locals_.size(); ++i)
        locals_[i] = std::move(params[i]);

    frames_.push_back(Frame{&chunk, 0, 0, 0});
    return execute(ctx);
}

VM::Result VM::resume(Value awaited, NativeCtx& ctx) {
    // El valor esperado ocupa el hueco que dejo la expresion `await`.
    push(std::move(awaited));
    return execute(ctx);
}

namespace {

// El try mas interno que cubre `pc`: entre varios anidados, el de rango menor.
const TryRange* find_handler(const Chunk& chunk, size_t pc) {
    const TryRange* best = nullptr;
    for (const auto& r : chunk.try_ranges) {
        if (pc < r.begin || pc >= r.end) continue;
        if (!best || (r.end - r.begin) < (best->end - best->begin)) best = &r;
    }
    return best;
}

Value error_value(const std::string& message) {
    Value::Dict d;
    d["message"] = Value::str(message);
    return Value::dict(std::move(d));
}

} // namespace

// Ejecuta y, si algo falla dentro de un `try`, salta a su `catch` y sigue.
// El error se entrega como un valor mas, en la cima de la pila.
VM::Result VM::execute(NativeCtx& ctx) {
    Result r = run_until_error(ctx);
    while (r.status == Status::Error) {
        // El pc del marco ya apunta a la siguiente instruccion, asi que la que fallo
        // anterior.
        // Un error sube por los marcos hasta encontrar un try que lo cubra: si
        // la funcion llamada no lo maneja, puede manejarlo quien la llamo.
        const TryRange* h = nullptr;
        while (!frames_.empty()) {
            Frame& f = frames_.back();
            h = find_handler(*f.chunk, f.pc - 1);
            if (h) break;
            stack_.resize(f.stack_base);
            locals_.resize(f.locals_base);
            frames_.pop_back();
        }
        if (!h) return r;

        // En un limite de sentencia la pila de operandos esta vacia, que es
        // donde puede empezar un try; limpiarla deja el estado consistente sin
        // tener que anotar profundidades.
        stack_.resize(frames_.back().stack_base);
        push(error_value(r.error));
        frames_.back().pc = h->catch_pc;
        r = run_until_error(ctx);
    }
    return r;
}

VM::Result VM::run_until_error(NativeCtx& ctx) {
    // El contador se reinicia en cada tramo: un bucle de SSE legitimo puede
    // estar horas vivo, pero entre dos suspensiones no debe dar mas de kStepLimit.
    long long steps = 0;

    while (!frames_.empty()) {
        Frame&       frame = frames_.back();
        const Chunk& chunk = *frame.chunk;

        // Una funcion que se acaba sin `return` devuelve null a quien la llamo.
        if (frame.pc >= chunk.code.size()) {
            size_t lbase = frame.locals_base, sbase = frame.stack_base;
            frames_.pop_back();
            if (frames_.empty()) return Result{};
            stack_.resize(sbase);
            locals_.resize(lbase);
            push(Value::null());
            continue;
        }

        if (++steps > kStepLimit)
            return fail("el handler supero el limite de pasos: bucle infinito?",
                        chunk.code[frame.pc].loc);

        const Instr& in = chunk.code[frame.pc++];

        switch (in.op) {
            case Op::Const:
                push(chunk.constants[in.operand]);
                break;

            case Op::LoadLocal:
                push(locals_[frame.locals_base + in.operand]);
                break;

            case Op::StoreLocal:
                locals_[frame.locals_base + in.operand] = pop();
                break;

            case Op::Pop:
                pop();
                break;

            case Op::Add: {
                Value b = pop(), a = pop();
                // '+' exige que AMBOS lados sean del mismo tipo. Dos cadenas
                // concatenan, dos numeros suman, dos listas se unen.
                //
                // 1 + "1" es un error, no "11": operar entre tipos distintos es
                // justo lo que Odio no quiere heredar de JavaScript. Para unir
                // un numero a una cadena hay que decirlo: "n = " + str(n).
                if (a.is_str() && b.is_str()) {
                    push(Value::str(a.as_str() + b.as_str()));
                } else if (a.is_str() || b.is_str()) {
                    return fail(std::string("no se puede sumar ") + a.type_name() +
                                " y " + b.type_name() +
                                "; para concatenar usa str(): \"...\" + str(x)", in.loc);
                } else if (numeric_pair(a, b)) {
                    if (a.is_int() && b.is_int())
                        push(Value::integer(a.as_int() + b.as_int()));
                    else
                        push(Value::real(a.as_float() + b.as_float()));
                } else if (a.is_list() && b.is_list()) {
                    Value::List out = a.as_list();
                    for (const auto& v : b.as_list()) out.push_back(v);
                    push(Value::list(std::move(out)));
                } else {
                    return fail(std::string("no se puede sumar ") + a.type_name() +
                                " y " + b.type_name(), in.loc);
                }
                break;
            }

            case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
                Value b = pop(), a = pop();
                if (!numeric_pair(a, b))
                    return fail(std::string("operacion aritmetica entre ") +
                                a.type_name() + " y " + b.type_name(), in.loc);

                bool ints = a.is_int() && b.is_int();
                if (in.op == Op::Mod) {
                    if (!ints) return fail("'%' solo aplica a enteros", in.loc);
                    if (b.as_int() == 0) return fail("modulo por cero", in.loc);
                    push(Value::integer(a.as_int() % b.as_int()));
                    break;
                }
                if (in.op == Op::Div) {
                    if (b.as_float() == 0) return fail("division por cero", in.loc);
                    if (ints && a.as_int() % b.as_int() == 0)
                        push(Value::integer(a.as_int() / b.as_int()));
                    else
                        push(Value::real(a.as_float() / b.as_float()));
                    break;
                }
                if (ints) {
                    long long x = a.as_int(), y = b.as_int();
                    push(Value::integer(in.op == Op::Sub ? x - y : x * y));
                } else {
                    double x = a.as_float(), y = b.as_float();
                    push(Value::real(in.op == Op::Sub ? x - y : x * y));
                }
                break;
            }

            case Op::Neg: {
                Value a = pop();
                if (a.is_int())        push(Value::integer(-a.as_int()));
                else if (a.is_float()) push(Value::real(-a.as_float()));
                else return fail(std::string("no se puede negar ") + a.type_name(), in.loc);
                break;
            }

            case Op::Eq: { Value b = pop(), a = pop(); push(Value::boolean(a.equals(b)));  break; }
            case Op::Ne: { Value b = pop(), a = pop(); push(Value::boolean(!a.equals(b))); break; }

            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                Value b = pop(), a = pop();
                bool ok = false;
                bool r  = compare(a, b, in.op, ok);
                if (!ok)
                    return fail(std::string("no se pueden comparar ") + a.type_name() +
                                " y " + b.type_name(), in.loc);
                push(Value::boolean(r));
                break;
            }

            case Op::Not:
                push(Value::boolean(!pop().truthy()));
                break;

            case Op::Jump:
                frame.pc = in.operand;
                break;

            case Op::JumpIfFalse:
                if (!pop().truthy()) frame.pc = in.operand;
                break;

            case Op::JumpIfFalsePeek:
                if (!stack_.back().truthy()) frame.pc = in.operand; else pop();
                break;

            case Op::JumpIfTruePeek:
                if (stack_.back().truthy()) frame.pc = in.operand; else pop();
                break;

            case Op::MakeList: {
                Value::List l;
                l.resize(in.operand);
                for (uint32_t i = in.operand; i-- > 0;) l[i] = pop();
                push(Value::list(std::move(l)));
                break;
            }

            case Op::MakeDict: {
                Value::Dict d;
                // Los pares se apilan clave,valor en orden; se recogen al reves.
                std::vector<std::pair<Value, Value>> pairs(in.operand);
                for (uint32_t i = in.operand; i-- > 0;) {
                    Value v = pop();
                    Value k = pop();
                    pairs[i] = {std::move(k), std::move(v)};
                }
                for (auto& [k, v] : pairs) {
                    if (!k.is_str())
                        return fail(std::string("la clave de un Dict tiene que ser "
                                    "string, no ") + k.type_name(), in.loc);
                    d[k.as_str()] = std::move(v);
                }
                push(Value::dict(std::move(d)));
                break;
            }

            case Op::GetIndex: {
                Value idx = pop(), obj = pop();
                if (obj.is_list()) {
                    if (!idx.is_int())
                        return fail("el indice de una List tiene que ser int", in.loc);
                    long long i = idx.as_int();
                    auto& l = obj.as_list();
                    if (i < 0 || i >= (long long)l.size())
                        return fail("indice fuera de rango: " + std::to_string(i) +
                                    " (tamano " + std::to_string(l.size()) + ")", in.loc);
                    push(l[static_cast<size_t>(i)]);
                } else if (obj.is_dict()) {
                    if (!idx.is_str())
                        return fail("la clave de un Dict tiene que ser string", in.loc);
                    auto& d  = obj.as_dict();
                    auto  it = d.find(idx.as_str());
                    push(it == d.end() ? Value::null() : it->second);
                } else {
                    return fail(std::string("no se puede indexar ") + obj.type_name(), in.loc);
                }
                break;
            }

            // Un `for` siempre recorre una lista: un Dict se recorre por sus
            // claves, que es lo que espera quien viene de Python.
            case Op::IterList: {
                Value v = pop();
                if (v.is_list()) { push(std::move(v)); break; }
                if (v.is_dict()) {
                    Value::List keys;
                    keys.reserve(v.as_dict().size());
                    for (const auto& [k, _] : v.as_dict()) keys.push_back(Value::str(k));
                    push(Value::list(std::move(keys)));
                    break;
                }
                return fail(std::string("no se puede recorrer ") + v.type_name() +
                            " con 'for'", in.loc);
            }

            case Op::GetMember: {
                Value obj = pop();
                const std::string& name = chunk.constants[in.operand].as_str();
                if (!obj.is_dict())
                    return fail(std::string("'") + name + "' sobre " + obj.type_name() +
                                ", que no tiene campos", in.loc);
                auto& d  = obj.as_dict();
                auto  it = d.find(name);
                push(it == d.end() ? Value::null() : it->second);
                break;
            }

            case Op::CallMethod: {
                uint32_t name_k = in.operand >> 8;
                int      argc   = static_cast<int>(in.operand & 0xFF);

                std::vector<Value> args(static_cast<size_t>(argc));
                for (int i = argc; i-- > 0;) args[static_cast<size_t>(i)] = pop();
                Value recv = pop();

                std::string error;
                Value out = call_method(ctx, recv, chunk.constants[name_k].as_str(),
                                        args, error);
                if (!error.empty()) return fail(std::move(error), in.loc);
                push(std::move(out));
                break;
            }

            case Op::CallNative: {
                int id   = static_cast<int>(in.operand >> 8);
                int argc = static_cast<int>(in.operand & 0xFF);

                std::vector<Value> args(static_cast<size_t>(argc));
                for (int i = argc; i-- > 0;) args[static_cast<size_t>(i)] = pop();

                std::string error;
                Value out = native_at(id).fn(ctx, args, error);
                if (!error.empty()) return fail(std::move(error), in.loc);
                push(std::move(out));
                break;
            }

            // El VM no sabe esperar: recoge los argumentos, se detiene, y deja
            // que el driver haga el co_await de verdad sobre el motor.  Al
            // volver, resume() apila el resultado y el marco sigue donde estaba.
            case Op::CallAsync: {
                int id   = static_cast<int>(in.operand >> 8);
                int argc = static_cast<int>(in.operand & 0xFF);

                Result r;
                r.status     = Status::Suspended;
                r.await_id   = id;
                r.await_args.resize(static_cast<size_t>(argc));
                for (int i = argc; i-- > 0;) r.await_args[static_cast<size_t>(i)] = pop();
                return r;
            }

            case Op::CallFunction: {
                size_t index = in.operand >> 8;
                int    argc  = static_cast<int>(in.operand & 0xFF);

                if (!functions_ || index >= functions_->size())
                    return fail("funcion no encontrada", in.loc);
                if (frames_.size() >= kMaxFrames)
                    return fail("demasiada recursion: mas de " +
                                std::to_string(kMaxFrames) + " llamadas anidadas",
                                in.loc);

                const Chunk& callee = *(*functions_)[index];

                std::vector<Value> args(static_cast<size_t>(argc));
                for (int i = argc; i-- > 0;) args[static_cast<size_t>(i)] = pop();

                Frame nf;
                nf.chunk       = &callee;
                nf.pc          = 0;
                nf.locals_base = locals_.size();
                nf.stack_base  = stack_.size();

                locals_.resize(locals_.size() +
                               static_cast<size_t>(callee.num_locals), Value::null());
                for (size_t i = 0; i < args.size(); ++i)
                    locals_[nf.locals_base + i] = std::move(args[i]);

                frames_.push_back(nf);
                break;
            }

            case Op::Return: {
                Value  v     = pop();
                size_t lbase = frame.locals_base, sbase = frame.stack_base;
                frames_.pop_back();
                if (frames_.empty()) {
                    Result r;
                    r.status = Status::Done;
                    r.value  = std::move(v);
                    return r;
                }
                stack_.resize(sbase);
                locals_.resize(lbase);
                push(std::move(v));
                break;
            }

            case Op::ReturnNull: {
                size_t lbase = frame.locals_base, sbase = frame.stack_base;
                frames_.pop_back();
                if (frames_.empty()) return Result{};
                stack_.resize(sbase);
                locals_.resize(lbase);
                push(Value::null());
                break;
            }
        }
    }

    return Result{};
}

} // namespace odio
