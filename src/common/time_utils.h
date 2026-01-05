#pragma once

#include <chrono>
#include <string>

namespace dc {
namespace common {

// Formats a time_point in UTC using ISO 8601 (e.g. 2025-01-04T08:57:00Z).
std::string ToUtcIso8601(std::chrono::system_clock::time_point tp);

// Returns current UTC time as ISO 8601.
std::string NowUtcIso8601();

}  // namespace common
}  // namespace dc
