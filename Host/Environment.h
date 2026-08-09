#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace Environment {

inline std::optional<std::string> read(const char* name) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }

    std::string result(value);
    free(value);
    return result;
}

} // namespace Environment
