#pragma once
#include <nlohmann/json.hpp>

/**
 * OSODIO_SCHEMA(Type, Fields...)
 *
 * This macro automatically provides JSON serialization (to_json) and
 * deserialization (from_json) for the given struct.
 *
 * Example:
 *   struct User { std::string name; int age; };
 *   OSODIO_SCHEMA(User, name, age);
 */

#define OSODIO_SCHEMA(Type, ...) \
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, __VA_ARGS__)
