#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "api_client.hpp"
#include "common/env.hpp"
#include "formatters.hpp"

namespace dc {
namespace cli {

using json = nlohmann::json;

struct GlobalOptions {
    std::string base_url;
    std::string host;
    int port = 8080;
    int timeout_ms = 5000;
    int retries = 10;
    int retries_timeout = 5;
    bool json_output = false;
    bool verbose = false;
    bool help = false;
};

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool ConsumeOptionValue(const std::vector<std::string>& args,
                        std::size_t* index,
                        const std::string& name,
                        std::string* out,
                        std::string* error) {
    const std::string& arg = args[*index];
    const std::string prefix = name + "=";
    if (arg == name) {
        if (*index + 1 >= args.size()) {
            if (error) {
                *error = "Missing value for " + name;
            }
            return true;
        }
        *out = args[*index + 1];
        *index += 1;
        return true;
    }
    if (StartsWith(arg, prefix)) {
        *out = arg.substr(prefix.size());
        return true;
    }
    return false;
}

bool ConsumeBoolFlag(const std::vector<std::string>& args,
                     std::size_t* index,
                     const std::string& name,
                     bool* out,
                     std::string* error) {
    const std::string& arg = args[*index];
    const std::string prefix = name + "=";
    if (arg == name) {
        *out = true;
        return true;
    }
    if (StartsWith(arg, prefix)) {
        std::string value = arg.substr(prefix.size());
        std::string lower;
        lower.resize(value.size());
        std::transform(value.begin(), value.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "true" || lower == "1") {
            *out = true;
            return true;
        }
        if (lower == "false" || lower == "0") {
            *out = false;
            return true;
        }
        if (error) {
            *error = "Invalid boolean value for " + name;
        }
        return true;
    }
    return false;
}

std::optional<int> ParseInt(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

bool IsValidTaskId(const std::string& task_id) {
    if (task_id.empty()) {
        return false;
    }
    for (char c : task_id) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::string ReadTaskIdAsString(const json& payload) {
    if (!payload.contains("task_id")) {
        return "";
    }
    if (payload["task_id"].is_number_integer()) {
        return std::to_string(payload["task_id"].get<long long>());
    }
    if (payload["task_id"].is_string()) {
        return payload["task_id"].get<std::string>();
    }
    return "";
}

std::string EnsureScheme(const std::string& base_url) {
    if (StartsWith(base_url, "http://") || StartsWith(base_url, "https://")) {
        return base_url;
    }
    return "http://" + base_url;
}

GlobalOptions DefaultGlobalOptions() {
    GlobalOptions options;
    options.host = dc::common::GetEnvOrDefault("MASTER_HOST", "127.0.0.1");
    options.port = dc::common::GetEnvIntOrDefault("MASTER_PORT", 8080);
    return options;
}

std::string BuildBaseUrl(const GlobalOptions& options) {
    if (!options.base_url.empty()) {
        return EnsureScheme(options.base_url);
    }
    std::string host = options.host.empty() ? "127.0.0.1" : options.host;
    int port = options.port > 0 ? options.port : 8080;
    return "http://" + host + ":" + std::to_string(port);
}

bool ParseGlobalOptions(const std::vector<std::string>& args,
                        GlobalOptions* out,
                        std::vector<std::string>* rest,
                        std::string* error) {
    if (!out) {
        return false;
    }
    *out = DefaultGlobalOptions();

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            out->help = true;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--base-url", &out->base_url, error)) {
            if (error && !error->empty()) {
                return false;
            }
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--host", &out->host, error)) {
            if (error && !error->empty()) {
                return false;
            }
            continue;
        }
        std::string value;
        if (ConsumeOptionValue(args, &i, "--port", &value, error)) {
            if (error && !error->empty()) {
                return false;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                if (error) {
                    *error = "Invalid port: " + value;
                }
                return false;
            }
            out->port = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--retries", &value, error)) {
            if (error && !error->empty()) {
                return false;
            }
            auto parsed = ParseInt(value);
            if (!parsed || *parsed <= 0) {
                if (error) {
                    *error = "Invalid retries: " + value;
                }
                return false;
            }
            out->retries = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--retries-timeout", &value, error)) {
            if (error && !error->empty()) {
                return false;
            }
            auto parsed = ParseInt(value);
            if (!parsed || *parsed <= 0) {
                if (error) {
                    *error = "Invalid retries-timeout: " + value;
                }
                return false;
            }
            out->retries_timeout = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--timeout-ms", &value, error)) {
            if (error && !error->empty()) {
                return false;
            }
            auto parsed = ParseInt(value);
            if (!parsed || *parsed <= 0) {
                if (error) {
                    *error = "Invalid timeout-ms: " + value;
                }
                return false;
            }
            out->timeout_ms = *parsed;
            continue;
        }
        if (ConsumeBoolFlag(args, &i, "--json", &out->json_output, error)) {
            if (error && !error->empty()) {
                return false;
            }
            continue;
        }
        if (ConsumeBoolFlag(args, &i, "--verbose", &out->verbose, error)) {
            if (error && !error->empty()) {
                return false;
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            if (error) {
                *error = "Unknown option: " + arg;
            }
            return false;
        }
        rest->assign(args.begin() + static_cast<long>(i), args.end());
        return true;
    }

    rest->clear();
    return true;
}

void PrintUsage() {
    std::cout
        << "Usage: dc_cli [global options] <tasks|agents> <command> [options]\n\n"
        << "Global options:\n"
        << "  --base-url <url>       Base URL, e.g. http://127.0.0.1:8080\n"
        << "  --host <host>          Master host (default: env MASTER_HOST)\n"
        << "  --port <port>          Master port (default: env MASTER_PORT)\n"
        << "  --timeout-ms <ms>      HTTP timeout in milliseconds (default: 5000)\n"
        << "  --retries <times>      Retries if connection faiiled (default: 10)\n"
        << "  --retries-timeout <s>  Retries cooldown if connection faiiled (default: 5)\n"
        << "  --json                 Print JSON responses as-is\n"
        << "  --verbose              Print request info to stderr\n"
        << "  -h, --help             Show help\n\n"
        << "Tasks commands:\n"
        << "  tasks submit|add --cmd <command> [--arg <arg>]... [--env K=V]...\n"
        << "                   [--timeout <sec>] [--os <os>] [--cpu <n>] [--ram <mb>]\n"
        << "                   [--label <label>]...\n"
        << "  tasks list|ls [--state <state>] [--agent-id <id>] [--limit N] [--offset N]\n"
        << "  tasks get|show <task_id>\n"
        << "  tasks cancel|rm <task_id>\n"
        << "  tasks logs|log <task_id> [--stream stdout|stderr] [--follow] [--poll-ms N]\n\n"
        << "Agents commands:\n"
        << "  agents list|ls [--status idle|busy|offline] [--limit N] [--offset N]\n"
        << "  agents get|show <agent_id>\n";
    std::cout.flush();
}

int ExitCodeForStatus(int status) {
    if (status >= 500) {
        return 3;
    }
    return 2;
}

bool PrintApiError(const std::string& body) {
    auto parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        return false;
    }
    if (!parsed.contains("error") || !parsed["error"].is_object()) {
        return false;
    }
    const auto& error = parsed["error"];
    std::string code = error.value("code", "");
    std::string message = error.value("message", "");
    if (code.empty() && message.empty()) {
        return false;
    }
    std::cerr << "Error";
    if (!code.empty()) {
        std::cerr << " (" << code << ")";
    }
    if (!message.empty()) {
        std::cerr << ": " << message;
    }
    std::cerr << '\n';
    if (error.contains("details") && !error["details"].is_null()) {
        std::cerr << "Details: " << error["details"].dump() << '\n';
    }
    return true;
}

int HandleErrorResult(const HttpResult& result, bool json_output) {
    if (!result.error.empty()) {
        std::cerr << result.error << '\n';
        return 4;
    }
    if (json_output) {
        if (!result.body.empty()) {
            std::cout << result.body << '\n';
        }
        return ExitCodeForStatus(result.status);
    }
    if (!PrintApiError(result.body)) {
        std::cerr << "HTTP " << result.status << "\n";
        if (!result.body.empty()) {
            std::cerr << result.body << '\n';
        }
    }
    return ExitCodeForStatus(result.status);
}

void AddParam(httplib::Params& params,
              const std::string& key,
              const std::optional<std::string>& value) {
    if (value && !value->empty()) {
        params.emplace(key, *value);
    }
}

std::optional<std::string> GetSinglePositional(const std::vector<std::string>& args) {
    if (args.empty()) {
        return std::nullopt;
    }
    return args[0];
}

int HandleTasksList(const ApiClientInterface& client,
                    const GlobalOptions& options,
                    const std::vector<std::string>& args) {
    std::optional<std::string> state;
    std::optional<std::string> agent_id;
    int limit = 50;
    int offset = 0;
    std::string error;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string value;
        if (ConsumeOptionValue(args, &i, "--state", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            state = value;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--agent-id", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            agent_id = value;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--limit", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                std::cerr << "Invalid limit: " << value << '\n';
                return 1;
            }
            limit = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--offset", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                std::cerr << "Invalid offset: " << value << '\n';
                return 1;
            }
            offset = *parsed;
            continue;
        }
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "Unknown option: " << args[i] << '\n';
            return 1;
        }
        std::cerr << "Unexpected argument: " << args[i] << '\n';
        return 1;
    }

    httplib::Params params;
    AddParam(params, "state", state);
    AddParam(params, "agent_id", agent_id);
    params.emplace("limit", std::to_string(limit));
    params.emplace("offset", std::to_string(offset));

    if (options.verbose) {
        std::cerr << "GET /api/v1/tasks" << '\n';
    }
    auto result = client.Get("/api/v1/tasks", params);
    if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        return HandleErrorResult(result, options.json_output);
    }

    if (options.json_output) {
        std::cout << result.body << '\n';
        return 0;
    }

    auto parsed = json::parse(result.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("tasks")) {
        std::cerr << "Invalid response" << '\n';
        return 4;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& task : parsed["tasks"]) {
        rows.push_back({ReadTaskIdAsString(task), task.value("state", "")});
    }
    PrintTable(std::cout, {"Task ID", "State"}, rows);
    return 0;
}

int HandleTasksGet(const ApiClientInterface& client,
                   const GlobalOptions& options,
                   const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "Expected exactly one task_id" << '\n';
        return 1;
    }
    auto task_id = GetSinglePositional(args);
    if (!task_id) {
        std::cerr << "Missing task_id" << '\n';
        return 1;
    }
    if (!IsValidTaskId(*task_id)) {
        std::cerr << "Invalid task_id" << '\n';
        return 1;
    }

    std::string path = "/api/v1/tasks/" + *task_id;
    if (options.verbose) {
        std::cerr << "GET " << path << '\n';
    }
    auto result = client.Get(path);
    if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        return HandleErrorResult(result, options.json_output);
    }

    if (options.json_output) {
        std::cout << result.body << '\n';
        return 0;
    }

    auto parsed = json::parse(result.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("task")) {
        std::cerr << "Invalid response" << '\n';
        return 4;
    }
    const auto& task = parsed["task"];
    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back("task_id", ReadTaskIdAsString(task));
    rows.emplace_back("state", task.value("state", ""));
    rows.emplace_back("command", task.value("command", ""));
    if (task.contains("args")) {
        rows.emplace_back("args", task["args"].dump());
    }
    if (task.contains("env")) {
        rows.emplace_back("env", task["env"].dump());
    }
    if (task.contains("timeout_sec")) {
        rows.emplace_back("timeout_sec", task["timeout_sec"].dump());
    }
    if (task.contains("constraints")) {
        rows.emplace_back("constraints", task["constraints"].dump());
    }
    if (task.contains("assigned_agent")) {
        rows.emplace_back("assigned_agent", task.value("assigned_agent", ""));
    }
    rows.emplace_back("created_at", task.value("created_at", ""));
    if (task.contains("started_at")) {
        rows.emplace_back("started_at", task.value("started_at", ""));
    }
    if (task.contains("finished_at")) {
        rows.emplace_back("finished_at", task.value("finished_at", ""));
    }
    if (task.contains("exit_code")) {
        rows.emplace_back("exit_code", task["exit_code"].dump());
    }
    if (task.contains("error_message")) {
        rows.emplace_back("error_message", task.value("error_message", ""));
    }
    PrintKeyValueTable(std::cout, rows);
    return 0;
}

int HandleTasksCancel(const ApiClientInterface& client,
                      const GlobalOptions& options,
                      const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "Expected exactly one task_id" << '\n';
        return 1;
    }
    auto task_id = GetSinglePositional(args);
    if (!task_id) {
        std::cerr << "Missing task_id" << '\n';
        return 1;
    }
    if (!IsValidTaskId(*task_id)) {
        std::cerr << "Invalid task_id" << '\n';
        return 1;
    }

    std::string path = "/api/v1/tasks/" + *task_id + "/cancel";
    if (options.verbose) {
        std::cerr << "POST " << path << '\n';
    }
    auto result = client.Post(path, "{}", "application/json; charset=utf-8");
    if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        return HandleErrorResult(result, options.json_output);
    }

    if (options.json_output) {
        std::cout << result.body << '\n';
        return 0;
    }

    PrintKeyValueTable(std::cout, {std::make_pair("status", "ok")});
    return 0;
}

int HandleTasksSubmit(const ApiClientInterface& client,
                      const GlobalOptions& options,
                      const std::vector<std::string>& args) {
    std::string command;
    std::vector<std::string> cmd_args;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> labels;
    std::optional<int> timeout;
    std::optional<int> cpu_cores;
    std::optional<int> ram_mb;
    std::string os;
    std::string error;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string value;
        if (ConsumeOptionValue(args, &i, "--cmd", &value, &error) ||
            ConsumeOptionValue(args, &i, "--command", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            command = value;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--arg", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            cmd_args.push_back(value);
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--env", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto pos = value.find('=');
            if (pos == std::string::npos || pos == 0) {
                std::cerr << "Invalid env, expected KEY=VALUE: " << value << '\n';
                return 1;
            }
            env.emplace_back(value.substr(0, pos), value.substr(pos + 1));
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--timeout", &value, &error) ||
            ConsumeOptionValue(args, &i, "--timeout-sec", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed || *parsed < 0) {
                std::cerr << "Invalid timeout: " << value << '\n';
                return 1;
            }
            timeout = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--os", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            os = value;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--cpu", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                std::cerr << "Invalid cpu: " << value << '\n';
                return 1;
            }
            cpu_cores = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--ram", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                std::cerr << "Invalid ram: " << value << '\n';
                return 1;
            }
            ram_mb = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--label", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            labels.push_back(value);
            continue;
        }
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "Unknown option: " << args[i] << '\n';
            return 1;
        }
        std::cerr << "Unexpected argument: " << args[i] << '\n';
        return 1;
    }

    if (command.empty()) {
        std::cerr << "Missing required --cmd" << '\n';
        return 1;
    }

    json body;
    body["command"] = command;
    body["args"] = json::array();
    for (const auto& arg : cmd_args) {
        body["args"].push_back(arg);
    }
    json env_json = json::object();
    for (const auto& entry : env) {
        env_json[entry.first] = entry.second;
    }
    body["env"] = env_json;
    if (timeout) {
        body["timeout_sec"] = *timeout;
    }

    json constraints = json::object();
    if (!os.empty()) {
        constraints["os"] = os;
    }
    if (cpu_cores) {
        constraints["cpu_cores"] = *cpu_cores;
    }
    if (ram_mb) {
        constraints["ram_mb"] = *ram_mb;
    }
    if (!labels.empty()) {
        constraints["labels"] = labels;
    }
    if (!constraints.empty()) {
        body["constraints"] = constraints;
    }

    if (options.verbose) {
        std::cerr << "POST /api/v1/tasks" << '\n';
    }
    auto result = client.Post("/api/v1/tasks", body.dump(), "application/json; charset=utf-8");
    if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        return HandleErrorResult(result, options.json_output);
    }

    if (options.json_output) {
        std::cout << result.body << '\n';
        return 0;
    }

    auto parsed = json::parse(result.body, nullptr, false);
    if (parsed.is_discarded()) {
        std::cerr << "Invalid response" << '\n';
        return 4;
    }
    std::string task_id = ReadTaskIdAsString(parsed);
    if (task_id.empty()) {
        std::cerr << "Invalid response: missing task_id" << '\n';
        return 4;
    }

    PrintKeyValueTable(std::cout, {std::make_pair("task_id", task_id)});
    return 0;
}

int HandleTasksLogs(const ApiClientInterface& client,
                    const GlobalOptions& options,
                    const std::vector<std::string>& args) {
    std::string stream = "stdout";
    bool follow = false;
    int poll_ms = 500;
    std::string error;

    if (args.empty()) {
        std::cerr << "Missing task_id" << '\n';
        return 1;
    }

    std::string task_id;
    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        if (!args[i].empty() && args[i][0] == '-') {
            break;
        }
        task_id = args[i];
        ++i;
        break;
    }
    if (task_id.empty()) {
        std::cerr << "Missing task_id" << '\n';
        return 1;
    }
    if (!IsValidTaskId(task_id)) {
        std::cerr << "Invalid task_id" << '\n';
        return 1;
    }

    for (; i < args.size(); ++i) {
        std::string value;
        if (ConsumeOptionValue(args, &i, "--stream", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            stream = value;
            continue;
        }
        if (ConsumeBoolFlag(args, &i, "--follow", &follow, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--poll-ms", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed || *parsed <= 0) {
                std::cerr << "Invalid poll-ms: " << value << '\n';
                return 1;
            }
            poll_ms = *parsed;
            continue;
        }
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "Unknown option: " << args[i] << '\n';
            return 1;
        }
        std::cerr << "Unexpected argument: " << args[i] << '\n';
        return 1;
    }

    if (stream != "stdout" && stream != "stderr") {
        std::cerr << "Invalid stream: " << stream << '\n';
        return 1;
    }

    std::string base_path = "/api/v1/tasks/" + task_id;
    if (!follow) {
        httplib::Params params;
        params.emplace("stream", stream);
        if (options.verbose) {
            std::cerr << "GET " << base_path << "/logs" << '\n';
        }
        auto result = client.Get(base_path + "/logs", params);
        if (!result.error.empty() || result.status < 200 || result.status >= 300) {
            return HandleErrorResult(result, options.json_output);
        }
        std::cout << result.body;
        return 0;
    }

    std::uint64_t offset = 0;
    while (true) {
        httplib::Params params;
        params.emplace("stream", stream);
        params.emplace("from", std::to_string(offset));
        if (options.verbose) {
            std::cerr << "GET " << base_path << "/logs:tail" << '\n';
        }
        auto result = client.Get(base_path + "/logs:tail", params);
        if (!result.error.empty() || result.status < 200 || result.status >= 300) {
            return HandleErrorResult(result, options.json_output);
        }
        if (!result.body.empty()) {
            std::cout << result.body;
            std::cout.flush();
        }
        auto it = result.headers.find("X-Log-Size");
        if (it != result.headers.end()) {
            try {
                offset = static_cast<std::uint64_t>(std::stoull(it->second));
            } catch (...) {
                offset += static_cast<std::uint64_t>(result.body.size());
            }
        } else {
            offset += static_cast<std::uint64_t>(result.body.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}

int HandleAgentsList(const ApiClientInterface& client,
                     const GlobalOptions& options,
                     const std::vector<std::string>& args) {
    std::optional<std::string> status;
    int limit = 50;
    int offset = 0;
    std::string error;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string value;
        if (ConsumeOptionValue(args, &i, "--status", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            status = value;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--limit", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                std::cerr << "Invalid limit: " << value << '\n';
                return 1;
            }
            limit = *parsed;
            continue;
        }
        if (ConsumeOptionValue(args, &i, "--offset", &value, &error)) {
            if (!error.empty()) {
                std::cerr << error << '\n';
                return 1;
            }
            auto parsed = ParseInt(value);
            if (!parsed) {
                std::cerr << "Invalid offset: " << value << '\n';
                return 1;
            }
            offset = *parsed;
            continue;
        }
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "Unknown option: " << args[i] << '\n';
            return 1;
        }
        std::cerr << "Unexpected argument: " << args[i] << '\n';
        return 1;
    }

    httplib::Params params;
    AddParam(params, "status", status);
    params.emplace("limit", std::to_string(limit));
    params.emplace("offset", std::to_string(offset));

    if (options.verbose) {
        std::cerr << "GET /api/v1/agents" << '\n';
    }
    auto result = client.Get("/api/v1/agents", params);
    if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        return HandleErrorResult(result, options.json_output);
    }

    if (options.json_output) {
        std::cout << result.body << '\n';
        return 0;
    }

    auto parsed = json::parse(result.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("agents")) {
        std::cerr << "Invalid response" << '\n';
        return 4;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& agent : parsed["agents"]) {
        rows.push_back({agent.value("agent_id", ""), agent.value("status", "")});
    }
    PrintTable(std::cout, {"Agent ID", "Status"}, rows);
    return 0;
}

int HandleAgentsGet(const ApiClientInterface& client,
                    const GlobalOptions& options,
                    const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "Expected exactly one agent_id" << '\n';
        return 1;
    }
    auto agent_id = GetSinglePositional(args);
    if (!agent_id) {
        std::cerr << "Missing agent_id" << '\n';
        return 1;
    }

    std::string path = "/api/v1/agents/" + *agent_id;
    if (options.verbose) {
        std::cerr << "GET " << path << '\n';
    }
    auto result = client.Get(path);
    if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        return HandleErrorResult(result, options.json_output);
    }

    if (options.json_output) {
        std::cout << result.body << '\n';
        return 0;
    }

    auto parsed = json::parse(result.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("agent")) {
        std::cerr << "Invalid response" << '\n';
        return 4;
    }

    const auto& agent = parsed["agent"];
    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back("agent_id", agent.value("agent_id", ""));
    rows.emplace_back("os", agent.value("os", ""));
    rows.emplace_back("version", agent.value("version", ""));
    if (agent.contains("resources")) {
        rows.emplace_back("resources", agent["resources"].dump());
    }
    rows.emplace_back("status", agent.value("status", ""));
    rows.emplace_back("last_heartbeat", agent.value("last_heartbeat", ""));
    PrintKeyValueTable(std::cout, rows);
    return 0;
}

int HandleTasks(const ApiClientInterface& client,
                const GlobalOptions& options,
                const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Missing tasks command" << '\n';
        return 1;
    }
    std::string sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "list" || sub == "ls") {
        return HandleTasksList(client, options, rest);
    }
    if (sub == "get" || sub == "show") {
        return HandleTasksGet(client, options, rest);
    }
    if (sub == "submit" || sub == "add") {
        return HandleTasksSubmit(client, options, rest);
    }
    if (sub == "cancel" || sub == "rm") {
        return HandleTasksCancel(client, options, rest);
    }
    if (sub == "logs" || sub == "log") {
        return HandleTasksLogs(client, options, rest);
    }

    std::cerr << "Unknown tasks command: " << sub << '\n';
    return 1;
}

int HandleAgents(const ApiClientInterface& client,
                 const GlobalOptions& options,
                 const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Missing agents command" << '\n';
        return 1;
    }
    std::string sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "list" || sub == "ls") {
        return HandleAgentsList(client, options, rest);
    }
    if (sub == "get" || sub == "show") {
        return HandleAgentsGet(client, options, rest);
    }

    std::cerr << "Unknown agents command: " << sub << '\n';
    return 1;
}

}  // namespace cli
}  // namespace dc

#ifndef DC_CLI_DISABLE_MAIN
int main(int argc, char** argv) {
    using dc::cli::ApiClient;
    using dc::cli::GlobalOptions;
    using dc::cli::HandleAgents;
    using dc::cli::HandleTasks;
    using dc::cli::ParseGlobalOptions;
    using dc::cli::PrintUsage;
    using dc::cli::BuildBaseUrl;

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    GlobalOptions options;
    std::vector<std::string> rest;
    std::string error;
    if (!ParseGlobalOptions(args, &options, &rest, &error)) {
        if (!error.empty()) {
            std::cerr << error << '\n';
        }
        PrintUsage();
        return 1;
    }
    if (options.help || rest.empty()) {
        PrintUsage();
        return options.help ? 0 : 1;
    }

    std::string top = rest[0];
    std::vector<std::string> command_args(rest.begin() + 1, rest.end());

    std::string base_url = BuildBaseUrl(options);
    bool failed = false;

    for(int attempt = 0; attempt < options.retries; attempt++) {
        failed = false;
        ApiClient client(base_url, options.timeout_ms);
        int res;
        if (top == "tasks" || top == "task") {
            res = HandleTasks(client, options, command_args);
        }
        if (top == "agents" || top == "agent") {
            res = HandleAgents(client, options, command_args);
        }
        if(!res) {
            return 0;
        }
        std::cout << "Attempt " << attempt + 1 << " failed" << std::endl;
        failed = true;
        sleep(options.retries_timeout);
    }

    if(failed) {
        std::cerr << "Connection retries failed";
        return 1;
    }

    std::cerr << "Unknown command: " << top << '\n';
    PrintUsage();
    return 1;
}
#endif  // DC_CLI_DISABLE_MAIN
