#pragma once
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "natives.hpp"
#include "value.hpp"

namespace odio {

// Maquina virtual de Odio.
//
// La pila de operandos y las variables locales viven aqui, no en la pila de
// C++.  Por eso un handler puede detenerse a mitad: `run()` devuelve
// Suspended con la operacion pendiente, quien lo llama hace el co_await de
// verdad sobre el motor, y `resume()` continua justo donde lo dejo.
//
// El VM no es una corrutina; el handler que lo conduce si lo es.  Esa division
// es lo que evita tener que reimplementar un planificador dentro del VM.
//
// Una instancia por peticion en vuelo, alojada en el marco de la corrutina del
// handler: dos VM suspendidos a la vez no pueden pisarse el estado.
class VM {
public:
    enum class Status { Done, Error, Suspended };

    struct Result {
        Status      status = Status::Done;
        Value       value;          // Done: valor devuelto por el handler
        std::string error;          // Error: motivo
        SourceLoc   error_loc;

        // Suspended: que hay que esperar antes de reanudar.
        int                await_id = -1;
        std::vector<Value> await_args;
    };

    // Arranca `chunk` con `params` en las primeras ranuras.
    Result start(const Chunk& chunk, std::vector<Value> params, NativeCtx& ctx);

    // Continua tras una suspension, dejando `awaited` como valor de la
    // expresion `await`.
    Result resume(Value awaited, NativeCtx& ctx);

    // Tope de pasos por handler: corta un bucle infinito en un .odio en vez de
    // dejar clavado un hilo del event loop, que se llevaria por delante todas
    // las conexiones de ese core.  Se reinicia en cada suspension, porque un
    // bucle de SSE legitimo puede correr durante horas.
    static constexpr long long kStepLimit = 50'000'000;

private:
    const Chunk*       chunk_ = nullptr;
    std::vector<Value> stack_;
    std::vector<Value> locals_;
    size_t             pc_ = 0;

    Result execute(NativeCtx& ctx);
    Result run_until_error(NativeCtx& ctx);

    void  push(Value v) { stack_.push_back(std::move(v)); }
    Value pop()         { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
};

} // namespace odio
