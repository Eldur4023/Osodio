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

template<typename T, typename... Fs>
void validate_field(const T& val, const std::string& field, std::vector<std::string>& errs, Fs&&... vs) {
    (vs(val, field, errs), ...);
}

} // namespace osodio
