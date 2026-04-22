#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include <mongocxx/v_noabi/mongocxx/exception/exception.hpp>

#include "common/env.hpp"
#include "common/logging.hpp"
#include "control_service.hpp"
#include "broker/mongo_broker.hpp"
#include "broker/pg_broker.hpp"

namespace {

std::string TrimWhitespace(const std::string& value) {
    const char* ws = " \t\r\n";
    std::size_t start = value.find_first_not_of(ws);
    if (start == std::string::npos) {
        return "";
    }
    std::size_t end = value.find_last_not_of(ws);
    return value.substr(start, end - start + 1);
}

bool SetEnvVar(const std::string& key, const std::string& value) {
#ifdef _WIN32
    return _putenv_s(key.c_str(), value.c_str()) == 0;
#else
    return setenv(key.c_str(), value.c_str(), 1) == 0;
#endif
}

bool LoadEnvFileToEnv(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) {
            *error = "Failed to open env file: " + path;
        }
        return false;
    }

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        std::string trimmed = TrimWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        auto pos = trimmed.find('=');
        if (pos == std::string::npos) {
            if (error) {
                *error =
                    "Invalid env line " + std::to_string(line_no) + ": missing '='";
            }
            return false;
        }

        std::string key = TrimWhitespace(trimmed.substr(0, pos));
        std::string value = TrimWhitespace(trimmed.substr(pos + 1));
        if (key.empty()) {
            if (error) {
                *error =
                    "Invalid env line " + std::to_string(line_no) + ": empty key";
            }
            return false;
        }

        if (!SetEnvVar(key, value)) {
            if (error) {
                *error = "Failed to set env var from line " +
                         std::to_string(line_no) + ": " + key;
            }
            return false;
        }
    }

    return true;
}

struct MasterArgs {
    std::optional<std::string> env_file;
};

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ParseBoolEnvValue(const std::string& value) {
    const std::string lower = ToLower(TrimWhitespace(value));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool ParseValidatedEnvInt(const char* key,
                          int default_value,
                          int min_value,
                          int* out,
                          std::string* error) {
    if (!out) {
        if (error) {
            *error = "Internal error: ParseValidatedEnvInt output pointer is null";
        }
        return false;
    }
    const char* raw = std::getenv(key);
    if (!raw) {
        *out = default_value;
        return true;
    }

    std::string value(raw);
    if (value.empty()) {
        if (error) {
            *error = std::string("Invalid ") + key + ": value cannot be empty";
        }
        return false;
    }

    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0') {
        if (error) {
            *error = std::string("Invalid ") + key + ": expected integer, got '" + value + "'";
        }
        return false;
    }
    if (parsed < static_cast<long>(min_value)) {
        if (error) {
            *error = std::string("Invalid ") + key + ": must be >= " +
                     std::to_string(min_value) + ", got " + std::to_string(parsed);
        }
        return false;
    }
    if (parsed > static_cast<long>(std::numeric_limits<int>::max())) {
        if (error) {
            *error = std::string("Invalid ") + key + ": value is too large";
        }
        return false;
    }

    *out = static_cast<int>(parsed);
    return true;
}

void PrintUsage() {
    std::cout << "Usage: dc_master [--env-file <path>]\n"
              << "Options:\n"
              << "  --env-file, -e PATH  Load environment variables from PATH before start\n"
              << "  --help, -h           Show this message\n";
}

bool ParseArgs(int argc, char* argv[], MasterArgs* args, std::string* error) {
    if (!args) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return false;
        } else if (arg == "--env-file" || arg == "-e") {
            if (i + 1 >= argc) {
                if (error) {
                    *error = "Missing value for --env-file";
                }
                return false;
            }
            args->env_file = argv[++i];
        } else if (arg.rfind("--env-file=", 0) == 0) {
            args->env_file = arg.substr(std::string("--env-file=").size());
        } else {
            if (error) {
                *error = "Unknown option: " + arg;
            }
            PrintUsage();
            return false;
        }
    }

    return true;
}

int NormalizeExitCode(int code) {
#if defined(_WIN32)
    return code;
#else
    if (WIFEXITED(code)) {
        return WEXITSTATUS(code);
    }
    return code;
#endif
}

bool IsCommandNotFound(int code) {
    return code == 127 || code == 9009;
}

int RunInitDbScript() {
    const std::string config_path = dc::common::GetEnvOrDefault("DB_CONFIG", "");
    const std::string preferred_python = dc::common::GetEnvOrDefault("INIT_DB_PYTHON", "");
    const std::string init_db_script =
        dc::common::GetEnvOrDefault("INIT_DB_SCRIPT", "scripts/init_pg.py");

    std::vector<std::string> candidates;
    if (!preferred_python.empty()) {
        candidates.push_back(preferred_python);
    } else {
        candidates.push_back("python3");
        candidates.push_back("python");
    }

    for (const auto& python_cmd : candidates) {
        std::string command = python_cmd + " \"" + init_db_script + "\"";
        if (!config_path.empty()) {
            command += " --config \"" + config_path + "\"";
        }

        spdlog::info("Running migrations: {}", command);
        int raw_code = std::system(command.c_str());
        int code = NormalizeExitCode(raw_code);

        if (IsCommandNotFound(code) && candidates.size() > 1) {
            continue;
        }
        return code;
    }

    return 127;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace dc::master;

    MasterArgs args;
    std::string arg_error;
    if (!ParseArgs(argc, argv, &args, &arg_error)) {
        if (!arg_error.empty()) {
            std::cerr << arg_error << std::endl;
            return 2;
        }
        return 0;
    }

    if (args.env_file) {
        std::string load_error;
        if (!LoadEnvFileToEnv(*args.env_file, &load_error)) {
            std::cerr << load_error << std::endl;
            return 2;
        }
    }

    const std::string log_dir = dc::common::GetEnvOrDefault("LOG_DIR", "logs");
    const std::string log_level =
        dc::common::GetEnvOrDefault("MASTER_LOG_LEVEL", "trace");
    const std::string log_file =
        dc::common::GetEnvOrDefault("MASTER_LOG_FILE", log_dir + "/master.log");
    const auto resolved_level =
        dc::common::ParseLogLevel(log_level, spdlog::level::trace);
    dc::common::InitLogging(log_file, resolved_level);
    spdlog::info("Logging initialized (level={}, file={})",
                 spdlog::level::to_string_view(resolved_level),
                 log_file);

    // Configuration is pulled from environment variables to match deployment style.
    MasterConfig config;
    config.host = dc::common::GetEnvOrDefault("MASTER_HOST", "0.0.0.0");
    config.port = dc::common::GetEnvIntOrDefault("MASTER_PORT", 8080);
    config.heartbeat_interval_sec = dc::common::GetEnvIntOrDefault("HEARTBEAT_SEC", 30);
    config.offline_after_sec = dc::common::GetEnvIntOrDefault("OFFLINE_SEC", 120);
    config.log_dir = log_dir;
    config.max_log_upload_bytes =
        static_cast<std::size_t>(dc::common::GetEnvIntOrDefault("MAX_LOG_UPLOAD_BYTES",
                                                                10 * 1024 * 1024));
    const bool skip_db_migration = ParseBoolEnvValue(
        dc::common::GetEnvOrDefault(
            "MASTER_SKIP_DB_MIGRATION",
            dc::common::GetEnvOrDefault("SKIP_DB_MIGRATION", "false")));

    dc::broker::DbConfig db;
    std::string reconnect_error;
    if (!ParseValidatedEnvInt("BROKER_RECONNECT_ATTEMPTS",
                              5,
                              1,
                              &db.reconnect_attempts,
                              &reconnect_error)) {
        spdlog::critical("{}", reconnect_error);
        return 2;
    }
    if (!ParseValidatedEnvInt("BROKER_RECONNECT_COOLDOWN_SEC",
                              2,
                              0,
                              &db.reconnect_cooldown_sec,
                              &reconnect_error)) {
        spdlog::critical("{}", reconnect_error);
        return 2;
    }

    const std::string backend = ToLower(dc::common::GetEnvOrDefault("DB_BACKEND", "postgres"));
    if (backend != "postgres" && backend != "mongo") {
        spdlog::critical("Invalid DB_BACKEND '{}'. Expected 'postgres' or 'mongo'.", backend);
        return 2;
    }

    db.host = dc::common::GetEnvOrDefault("DB_HOST", "localhost");
    db.port = dc::common::GetEnvOrDefault("DB_PORT", backend == "mongo" ? "27017" : "5432");
    db.dbname = dc::common::GetEnvOrDefault("DB_NAME", "");
    db.mongo_auth_source = dc::common::GetEnvOrDefault("DB_MONGO_AUTH_SOURCE", "admin");

    try {
        db.authMethod = dc::broker::getAuthMethod(dc::common::GetEnvOrDefault("DB_AUTHMODE", "password"));
    } catch(std::exception ex) {
        spdlog::critical(ex.what());
        return 2;
    }

    switch(db.authMethod) {
        case dc::broker::AuthentificationMethod::PASSWORD:
            db.user = dc::common::GetEnvOrDefault("DB_USER", "");
            db.password = dc::common::GetEnvOrDefault("DB_PASSWORD", "");
            if(db.user.empty() || db.password.empty()) {
                spdlog::critical("Missing user or password for password authentification");
                return 2;
            }
            break;
        case dc::broker::AuthentificationMethod::SSL:
            db.ssl.rootcert = dc::common::GetEnvOrDefault("DB_SSL_ROOTCERT", "");
            db.ssl.cert = dc::common::GetEnvOrDefault("DB_SSL_CERT", "");
            db.ssl.key = dc::common::GetEnvOrDefault("DB_SSL_KEY", "");
            if(db.ssl.rootcert.empty() || db.ssl.cert.empty() || db.ssl.key.empty()) {
                spdlog::critical("Missing rootcert, cert or user for key for ssl authentification");
                return 2;
            }
            break;
        default:
            throw std::runtime_error("Unexpected case");
    }

    // Ensure log root exists even if no task logs are present yet.
    std::error_code ec;
    std::filesystem::create_directories(config.log_dir, ec);
    if (ec) {
        spdlog::error("Failed to create LOG_DIR: {}", config.log_dir);
        return 2;
    }

    if (skip_db_migration) {
        spdlog::warn("DB migration step skipped by MASTER_SKIP_DB_MIGRATION/SKIP_DB_MIGRATION.");
    } else {
        // Run migrations before broker startup.
        int init_code = RunInitDbScript();
        if (init_code != 0) {
            spdlog::critical("Migrations failed with code {}", init_code);
            return init_code;
        }
    }

    LogStore log_store(config.log_dir);
    dc::broker::Broker* broker = nullptr;
    if (backend == "mongo") {
        try {
            broker = new dc::broker::MongoBroker(db);
        } catch (const mongocxx::exception& ex) {
            spdlog::critical("Failed to initialize Mongo broker for URI '{}': {}",
                             db.host + ":" + db.port,
                             ex.what());
            return 2;
        } catch (const std::exception& ex) {
            spdlog::critical("Failed to initialize Mongo broker for URI '{}': {}",
                             db.host + ":" + db.port,
                             ex.what());
            return 2;
        }
    } else {
        broker = new dc::broker::PgBroker(db);
    }
    ControlService service(std::move(config), broker, std::move(log_store));
    return service.Run();
}
