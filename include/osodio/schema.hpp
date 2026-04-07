#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include "validation.hpp"

// ─── OSODIO_SCHEMA ────────────────────────────────────────────────────────────
// Genera from_json / to_json para una struct usando nlohmann/json.
// Uso: fuera del struct, después de su definición.
//
//   struct User { std::string name; int age; };
//   OSODIO_SCHEMA(User, name, age)
//
#define OSODIO_SCHEMA(Type, ...) \
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, __VA_ARGS__)

// ─── OSODIO_VALIDATE ─────────────────────────────────────────────────────────
// Define reglas de validación como una friend free-function para que ADL la
// encuentre cuando el código haga: validate(user)
//
// Uso: DENTRO del struct body. Los checks usan los campos directamente (this->).
//
//   struct User {
//       std::string name; int age;
//       OSODIO_VALIDATE(User,
//           check(name.size() > 0, "Name cannot be empty"),
//           check(age >= 18,       "Must be at least 18")
//       )
//   };
//
// Internamente define:
//   - _validate_impl(): member const que corre las reglas usando this->field
//   - friend validate(const User&): free function encontrable por ADL
//
#define OSODIO_VALIDATE(Type, ...)                                       \
    void _validate_impl(std::vector<std::string>& _errors) const {      \
        __VA_ARGS__;                                                     \
    }                                                                    \
    friend void validate(const Type& _self) {                           \
        std::vector<std::string> _errors;                               \
        _self._validate_impl(_errors);                                  \
        if (!_errors.empty())                                           \
            throw osodio::ValidationError(std::move(_errors));          \
    }

// check(condition, message)
// Se usa dentro de OSODIO_VALIDATE. Accede a los campos de `this` directamente.
#define check(cond, msg) \
    ((cond) ? (void)0 : _errors.push_back(msg))
