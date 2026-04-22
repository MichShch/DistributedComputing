#pragma once

#include <optional>
#include <string>

namespace dc {
namespace common {

// Reads an environment variable; returns default_value when it is not set.
std::string GetEnvOrDefault(const char* key, const std::string& default_value);

// Reads an environment variable as integer; returns default_value on missing/invalid.
int GetEnvIntOrDefault(const char* key, int default_value);

}  // namespace common
}  // namespace dc
