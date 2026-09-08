#pragma once

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace owngrove {

inline constexpr int kDefaultPort = 8787;

inline std::optional<std::string> readCompatEnv(const char* new_name, const char* old_name)
{
    const char* new_value = std::getenv(new_name);
    if (new_value != nullptr && *new_value != '\0') {
        return std::string(new_value);
    }

    const char* old_value = std::getenv(old_name);
    if (old_value != nullptr && *old_value != '\0') {
        std::cerr << "Warning: " << old_name << " is deprecated. Use " << new_name
                  << " instead." << std::endl;
        return std::string(old_value);
    }

    return std::nullopt;
}

inline bool parsePort(const char* text, int& port)
{
    if (text == nullptr || *text < '0' || *text > '9') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value < 1UL || value > 65535UL) {
        return false;
    }

    port = static_cast<int>(value);
    return true;
}

}  // namespace owngrove
