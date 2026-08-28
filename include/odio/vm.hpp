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
// C++.  Ejecutar un handler no recursa sobre el marco nativo, que es la
// condicion para poder suspender en `await` cuando llegue: bastara con guardar
// pc, stack_ y locals_ y reanudarlos.
//
// Hay una instancia por hilo de event loop y no comparte nada con las demas.
class VM {
public:
    struct Result {
        bool        ok = true;
        Value       value;          // valor devuelto por el handler
        std::string error;          // motivo, si ok == false
        SourceLoc   error_loc;
    };

    // Ejecuta `chunk` con `params` ya colocados en las primeras ranuras.
    Result run(const Chunk& chunk, std::vector<Value> params, NativeCtx& ctx);

    // Tope de pasos por handler: corta un bucle infinito en un .odio en vez de
    // dejar clavado un hilo del event loop, que se llevaria por delante todas
    // las conexiones de ese core.
    static constexpr long long kStepLimit = 50'000'000;

private:
    std::vector<Value> stack_;
    std::vector<Value> locals_;

    void  push(Value v) { stack_.push_back(std::move(v)); }
    Value pop()         { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
};

} // namespace odio
