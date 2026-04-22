#include "api_mappers.hpp"

#include <cctype>
#include <limits>

#include "broker/status.hpp"

namespace dc {
using namespace broker;
namespace master {
namespace api {

namespace {

std::optional<std::string> OptionalNonEmptyStringField(const nlohmann::json& body,
                                                       const char* key) {
    if (!body.contains(key) || !body[key].is_string()) {
        return std::nullopt;
    }
    const std::string value = body[key].get<std::string>();
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

nlohmann::json SanitizeStringArray(const nlohmann::json& value) {
    nlohmann::json sanitized = nlohmann::json::array();
    if (!value.is_array()) {
        return sanitized;
    }
    for (const auto& item : value) {
        if (item.is_string()) {
            sanitized.push_back(item);
        }
    }
    return sanitized;
}

}  // namespace

bool ParseAgentInput(const std::string& agent_id,
                     const nlohmann::json& body,
                     AgentInput* out,
                     std::string* error) {
    if (!out) {
        if (error) {
            *error = "Invalid payload";
        }
        return false;
    }

    if (!body.contains("os") || !body["os"].is_string() ||
        !body.contains("version") || !body["version"].is_string() ||
        !body.contains("resources") || !body["resources"].is_object()) {
        if (error) {
            *error = "Missing required fields";
        }
        return false;
    }

    const auto& resources = body["resources"];
    if (!resources.contains("cpu_cores") || !resources["cpu_cores"].is_number_integer() ||
        !resources.contains("ram_mb") || !resources["ram_mb"].is_number_integer() ||
        !resources.contains("slots") || !resources["slots"].is_number_integer()) {
        if (error) {
            *error = "Invalid resources payload";
        }
        return false;
    }

    out->agent_id = agent_id;
    out->os = body["os"].get<std::string>();
    out->version = body["version"].get<std::string>();
    out->cpu_cores = resources["cpu_cores"].get<int>();
    out->ram_mb = resources["ram_mb"].get<int>();
    out->slots = resources["slots"].get<int>();
    return true;
}

bool ParseAgentHeartbeat(const std::string& agent_id,
                         const nlohmann::json& body,
                         AgentHeartbeat* out,
                         std::string* error) {
    if (!out) {
        if (error) {
            *error = "Invalid payload";
        }
        return false;
    }
    if (!body.contains("status") || !body["status"].is_string()) {
        if (error) {
            *error = "Missing status";
        }
        return false;
    }
    std::string status = body["status"].get<std::string>();
    auto parsed = AgentStatusFromApi(status);
    if (!parsed) {
        if (error) {
            *error = "Invalid agent status";
        }
        return false;
    }
    out->agent_id = agent_id;
    out->status = *parsed;
    return true;
}

bool ParseTaskCreate(const nlohmann::json& body, TaskInput* out, std::string* error) {
    if (!out) {
        if (error) {
            *error = "Invalid payload";
        }
        return false;
    }
    if (!body.contains("command") || !body["command"].is_string()) {
        if (error) {
            *error = "Missing command";
        }
        return false;
    }

    out->command = body["command"].get<std::string>();
    out->args = body.contains("args") ? SanitizeStringArray(body["args"])
                                      : nlohmann::json::array();
    out->env = (body.contains("env") && body["env"].is_object()) ? body["env"]
                                                                 : nlohmann::json::object();
    if (body.contains("timeout_sec") && body["timeout_sec"].is_number_integer()) {
        out->timeout_sec = body["timeout_sec"].get<int>();
    } else {
        out->timeout_sec = std::nullopt;
    }
    if (body.contains("constraints") && body["constraints"].is_object()) {
        out->constraints = body["constraints"];
    } else {
        out->constraints = nlohmann::json::object();
    }
    return true;
}

bool ParseTaskStatusUpdate(const nlohmann::json& body,
                           TaskStatusUpdate* out,
                           std::string* error) {
    if (!out) {
        if (error) {
            *error = "Invalid payload";
        }
        return false;
    }
    if (!body.contains("state") || !body["state"].is_string()) {
        if (error) {
            *error = "Missing state";
        }
        return false;
    }

    std::string state = body["state"].get<std::string>();
    auto parsed = TaskStateFromApi(state);
    if (!parsed) {
        if (error) {
            *error = "Invalid task state";
        }
        return false;
    }
    out->state = *parsed;
    if (body.contains("exit_code") && body["exit_code"].is_number_integer()) {
        out->exit_code = body["exit_code"].get<int>();
    } else {
        out->exit_code = std::nullopt;
    }
    out->started_at = OptionalNonEmptyStringField(body, "started_at");
    out->finished_at = OptionalNonEmptyStringField(body, "finished_at");
    out->error_message = OptionalNonEmptyStringField(body, "error_message");
    return true;
}

nlohmann::json AgentRecordToJson(const AgentRecord& agent) {
    nlohmann::json payload;
    payload["agent_id"] = agent.agent_id;
    payload["os"] = agent.os;
    payload["version"] = agent.version;
    payload["resources"] = {
        {"cpu_cores", agent.cpu_cores},
        {"ram_mb", agent.ram_mb},
        {"slots", agent.slots},
    };
    payload["status"] = AgentStatusToApi(agent.status);
    payload["last_heartbeat"] = agent.last_heartbeat;
    return payload;
}

nlohmann::json AgentSummaryToJson(const AgentRecord& agent) {
    return nlohmann::json{{"agent_id", agent.agent_id},
                          {"status", AgentStatusToApi(agent.status)}};
}

nlohmann::json TaskRecordToJson(const TaskRecord& task) {
    nlohmann::json payload;
    payload["task_id"] = task.task_id;
    payload["state"] = TaskStateToApi(task.state);
    payload["command"] = task.command;
    payload["args"] = task.args;
    payload["env"] = task.env;
    if (task.timeout_sec) {
        payload["timeout_sec"] = *task.timeout_sec;
    }
    if (task.assigned_agent) {
        payload["assigned_agent"] = *task.assigned_agent;
    }
    payload["created_at"] = task.created_at;
    if (task.started_at) {
        payload["started_at"] = *task.started_at;
    }
    if (task.finished_at) {
        payload["finished_at"] = *task.finished_at;
    }
    if (task.exit_code) {
        payload["exit_code"] = *task.exit_code;
    }
    if (task.error_message) {
        payload["error_message"] = *task.error_message;
    }
    payload["constraints"] = task.constraints;
    return payload;
}

nlohmann::json TaskSummaryToJson(const TaskSummary& task) {
    return nlohmann::json{{"task_id", task.task_id},
                          {"state", TaskStateToApi(task.state)}};
}

nlohmann::json TaskDispatchToJson(const TaskDispatch& task) {
    nlohmann::json payload;
    payload["task_id"] = task.task_id;
    payload["command"] = task.command;
    payload["args"] = task.args;
    payload["env"] = task.env;
    if (task.timeout_sec) {
        payload["timeout_sec"] = *task.timeout_sec;
    }
    if (!task.constraints.is_null()) {
        payload["constraints"] = task.constraints;
    }
    return payload;
}

std::optional<std::int64_t> ParseTaskId(const std::string& task_id) {
    if (task_id.empty()) {
        return std::nullopt;
    }
    for (char c : task_id) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }
    try {
        long long parsed = std::stoll(task_id);
        if (parsed <= 0) {
            return std::nullopt;
        }
        if (parsed > std::numeric_limits<std::int64_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

bool IsValidTaskStateTransition(TaskState from, TaskState to) {
    if (from == to) {
        return true;
    }
    switch (from) {
        case TaskState::Queued:
            return to == TaskState::Running || to == TaskState::Canceled;
        case TaskState::Running:
            return to == TaskState::Succeeded || to == TaskState::Failed ||
                   to == TaskState::Canceled;
        default:
            return false;
    }
}

}  // namespace api
}  // namespace master
}  // namespace dc
