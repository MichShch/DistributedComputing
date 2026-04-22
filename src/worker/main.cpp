#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "worker_app.hpp"

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
                *error = "Invalid env line " + std::to_string(line_no) + ": missing '='";
            }
            return false;
        }

        std::string key = TrimWhitespace(trimmed.substr(0, pos));
        std::string value = TrimWhitespace(trimmed.substr(pos + 1));
        if (key.empty()) {
            if (error) {
                *error = "Invalid env line " + std::to_string(line_no) + ": empty key";
            }
            return false;
        }

        if (!SetEnvVar(key, value)) {
            if (error) {
                *error = "Failed to set env var from line " + std::to_string(line_no) + ": " + key;
            }
            return false;
        }
    }

    return true;
}

struct WorkerArgs {
    bool run_once = false;
    std::optional<std::string> env_file;
    std::optional<std::string> agent_id;
};

void PrintUsage() {
    std::cout << "Usage: dc_worker [--once] [--env-file <path>] [--agent-id <id>]\n"
              << "Options:\n"
              << "  --once               Run a single heartbeat/poll cycle and exit\n"
              << "  --env-file, -e PATH  Load environment variables from PATH before start\n"
              << "  --agent-id, -a ID    Override AGENT_ID with the provided value\n"
              << "  --help, -h           Show this message\n";
}

bool ParseArgs(int argc, char* argv[], WorkerArgs* args, std::string* error) {
    if (!args) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--once") {
            args->run_once = true;
        } else if (arg == "--help" || arg == "-h") {
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
        } else if (arg == "--agent-id" || arg == "-a") {
            if (i + 1 >= argc) {
                if (error) {
                    *error = "Missing value for --agent-id";
                }
                return false;
            }
            args->agent_id = argv[++i];
        } else if (arg.rfind("--agent-id=", 0) == 0) {
            args->agent_id = arg.substr(std::string("--agent-id=").size());
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

}  // namespace

int main(int argc, char* argv[]) {
    WorkerArgs args;
    std::string error;
    if (!ParseArgs(argc, argv, &args, &error)) {
        if (!error.empty()) {
            std::cerr << error << std::endl;
            return 2;
        }
        return 0;
    }

    if (args.env_file) {
        if (!LoadEnvFileToEnv(*args.env_file, &error)) {
            std::cerr << error << std::endl;
            return 2;
        }
    }

    if (args.agent_id) {
#ifdef _WIN32
        if (_putenv_s("AGENT_ID", args.agent_id->c_str()) != 0) {
            std::cerr << "Failed to set AGENT_ID from --agent-id" << std::endl;
            return 2;
        }
#else
        if (setenv("AGENT_ID", args.agent_id->c_str(), 1) != 0) {
            std::cerr << "Failed to set AGENT_ID from --agent-id" << std::endl;
            return 2;
        }
#endif
    }

    auto app = dc::worker::CreateWorkerAppFromEnv(&error);
    if (!app) {
        std::cerr << error << std::endl;
        return 2;
    }

    return app->Run(args.run_once);
}
