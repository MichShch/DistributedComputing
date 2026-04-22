#include "env.hpp"

#include <cstdlib>

namespace dc {
namespace common {

namespace {

std::optional<std::string> GetEnv(const char* key) {
    if (!key) {
        return std::nullopt;
    }
    const char* value = std::getenv(key);
    if (!value) {
        return std::nullopt;
    }
    return std::string(value);
}

}  // namespace

std::string GetEnvOrDefault(const char* key, const std::string& default_value) {
    auto value = GetEnv(key);
    return value ? *value : default_value;
}

int GetEnvIntOrDefault(const char* key, int default_value) {
    auto value = GetEnv(key);
    if (!value) {
        return default_value;
    }
    char* end = nullptr;
    long parsed = std::strtol(value->c_str(), &end, 10);
    if (!end || *end != '\0') {
        return default_value;
    }
    return static_cast<int>(parsed);
}

}  // namespace common
}  // namespace dc
