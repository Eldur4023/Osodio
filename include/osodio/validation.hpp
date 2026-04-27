#pragma once
#include <vector>
#include <string>
#include <stdexcept>
#include <functional>

namespace osodio {

struct ValidationError : public std::runtime_error {
    std::vector<std::string> messages;
    ValidationError(std::vector<std::string> m) 
        : std::runtime_error("Validation Error"), messages(std::move(m)) {}
};

// --- Validator builders ---

template<typename T>
auto min(T m) {
    return [m](const T& val, const std::string& field, std::vector<std::string>& errs) {
        if (val < m) errs.push_back(field + " must be at least " + std::to_string(m));
    };
}

template<typename T>
auto max(T m) {
    return [m](const T& val, const std::string& field, std::vector<std::string>& errs) {
        if (val > m) errs.push_back(field + " must be at most " + std::to_string(m));
    };
}

inline auto len_min(size_t m) {
    return [m](const std::string& val, const std::string& field, std::vector<std::string>& errs) {
        if (val.size() < m) errs.push_back(field + " length must be at least " + std::to_string(m));
    };
}

inline auto len_max(size_t m) {
    return [m](const std::string& val, const std::string& field, std::vector<std::string>& errs) {
        if (val.size() > m) errs.push_back(field + " length must be at most " + std::to_string(m));
    };
}

// --- Validation infrastructure ---

// Classic overload: explicit field name string.
//   validate_field(age, "age", errs, osodio::min(18));
template<typename T, typename... Fs>
void validate_field(const T& val, const std::string& field, std::vector<std::string>& errs, Fs&&... vs) {
    (vs(val, field, errs), ...);
}

#if __cpp_reflection
// Reflection overload: field name derived from the member reflection.
// Usage inside a struct member function:
//   osodio::validate_field<^^age>(*this, errs, osodio::min(18), osodio::max(120));
// The field name comes from the reflection — no string literal to maintain.
template<auto Member, typename T, typename... Fs>
void validate_field(const T& obj, std::vector<std::string>& errs, Fs&&... vs) {
    const std::string name(std::meta::identifier_of(Member));
    (vs(obj.[:Member:], name, errs), ...);
}
#endif // __cpp_reflection

} // namespace osodio
