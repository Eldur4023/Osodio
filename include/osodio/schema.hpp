#pragma once
#include <nlohmann/json.hpp>
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
 * Rules should be a list of check(field, validators...)
 */
#define OSODIO_VALIDATE(Type, ...) \
    inline void validate(const Type& x) { \
        std::vector<std::string> _errors; \
        __VA_ARGS__; \
        if (!_errors.empty()) throw osodio::ValidationError(std::move(_errors)); \
    }

/**
 * check(field, validators...)
 * Used inside OSODIO_VALIDATE to apply validators to a field.
 */
#define check(field, ...) \
    osodio::validate_field(x.field, #field, _errors, __VA_ARGS__)
