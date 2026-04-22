#pragma once

#include <cstddef>
#include <string>

#include <spdlog/spdlog.h>

namespace dc {
namespace common {

constexpr std::size_t kLogRotateSizeBytes = 10 * 1024 * 1024;
constexpr std::size_t kLogRotateFiles = 5;

spdlog::level::level_enum ParseLogLevel(const std::string& value,
                                        spdlog::level::level_enum fallback);

void InitLogging(const std::string& log_file,
                 spdlog::level::level_enum level,
                 std::size_t max_size_bytes = kLogRotateSizeBytes,
                 std::size_t max_files = kLogRotateFiles);

}  // namespace common
}  // namespace dc
