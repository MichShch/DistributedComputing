#include "logging.hpp"

#include <filesystem>
#include <iostream>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include "string_utils.hpp"

namespace fs = std::filesystem;

namespace dc {
namespace common {

spdlog::level::level_enum ParseLogLevel(const std::string& value,
                                        spdlog::level::level_enum fallback) {
    if (value.empty()) {
        return fallback;
    }
    std::string lower = ToLowerCopy(value);

    if (lower == "trace") {
        return spdlog::level::trace;
    }
    if (lower == "debug") {
        return spdlog::level::debug;
    }
    if (lower == "info") {
        return spdlog::level::info;
    }
    if (lower == "warn" || lower == "warning") {
        return spdlog::level::warn;
    }
    if (lower == "error" || lower == "err") {
        return spdlog::level::err;
    }
    if (lower == "critical" || lower == "crit") {
        return spdlog::level::critical;
    }
    if (lower == "off") {
        return spdlog::level::off;
    }
    return fallback;
}

void InitLogging(const std::string& log_file,
                 spdlog::level::level_enum level,
                 std::size_t max_size_bytes,
                 std::size_t max_files) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());

    if (!log_file.empty()) {
        try {
            fs::path path(log_file);
            if (path.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(path.parent_path(), ec);
            }
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, max_size_bytes, max_files);
            sinks.push_back(std::move(file_sink));
        } catch (const std::exception& ex) {
            std::cerr << "Failed to init log file '" << log_file << "': " << ex.what()
                      << std::endl;
        }
    }

    auto logger = std::make_shared<spdlog::logger>("master", sinks.begin(), sinks.end());
    logger->set_level(level);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(level);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%t] %v");
    spdlog::flush_on(spdlog::level::info);
}

}  // namespace common
}  // namespace dc
