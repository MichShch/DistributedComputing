#pragma once

#include <chrono>
#include <string>

namespace dc {
namespace common {

// Returns current UTC time as ISO 8601.
std::string NowUtcIso8601();

}  // namespace common
}  // namespace dc
