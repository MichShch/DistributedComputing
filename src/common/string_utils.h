#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace dc {
namespace common {

inline std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace common
}  // namespace dc
