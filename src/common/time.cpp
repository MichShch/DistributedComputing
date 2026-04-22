#include "time_utils.hpp"

#include <ctime>

namespace dc {
namespace common {

namespace {

std::string ToUtcIso8601(std::chrono::system_clock::time_point tp) {
    std::time_t raw = std::chrono::system_clock::to_time_t(tp);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &raw);
#else
    gmtime_r(&raw, &utc_tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
    return std::string(buffer);
}

}  // namespace

std::string NowUtcIso8601() {
    return ToUtcIso8601(std::chrono::system_clock::now());
}

}  // namespace common
}  // namespace dc
