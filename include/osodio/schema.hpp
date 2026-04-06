#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include "validation.hpp"

/**
 * OSODIO_SCHEMA(Type, Fields...)
 * Provides automatic JSON serialization/deserialization.
 */
#define OSODIO_SCHEMA(Type, ...) \
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, __VA_ARGS__)

/**
 * OSODIO_VALIDATE(Type, Rules...)
 * Defines validation rules for a struct.
 */
#define OSODIO_VALIDATE(Type, ...) \
    inline void validate(const Type& x) { \
        std::vector<std::string> _errors; \
        __VA_ARGS__; \
        if (!_errors.empty()) throw osodio::ValidationError(std::move(_errors)); \
    }

/**
 * check(condition, message)
 * Expression-based check to allow comma separation in OSODIO_VALIDATE.
 */
#define check(cond, msg) \
    ((cond) ? (void)0 : _errors.push_back(msg))

/**
 * validate_field(field, validators...)
 * For more complex, reusable validators.
 */
#define validate_field(field, ...) \
    osodio::validate_fields(x.field, #field, _errors, __VA_ARGS__)
