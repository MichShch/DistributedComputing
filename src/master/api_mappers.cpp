#include "api_mappers.h"

#include <cctype>

#include "status.h"

namespace dc {
namespace master {
namespace api {

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
    if (!body.contains("task_id") || !body["task_id"].is_string()) {
        if (error) {
            *error = "Missing task_id";
        }
        return false;
    }
    if (!body.contains("command") || !body["command"].is_string()) {
        if (error) {
            *error = "Missing command";
        }
        return false;
    }

    out->task_id = body["task_id"].get<std::string>();
    if (!IsValidTaskId(out->task_id)) {
        if (error) {
            *error = "Invalid task_id";
        }
        return false;
    }
    out->command = body["command"].get<std::string>();
    out->args = (body.contains("args") && body["args"].is_array()) ? body["args"]
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
    if (body.contains("started_at") && body["started_at"].is_string()) {
        out->started_at = body["started_at"].get<std::string>();
    } else {
        out->started_at = std::nullopt;
    }
    if (body.contains("finished_at") && body["finished_at"].is_string()) {
        out->finished_at = body["finished_at"].get<std::string>();
    } else {
        out->finished_at = std::nullopt;
    }
    if (body.contains("error_message") && body["error_message"].is_string()) {
        out->error_message = body["error_message"].get<std::string>();
    } else {
        out->error_message = std::nullopt;
    }
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

bool IsValidTaskId(const std::string& task_id) {
    if (task_id.empty() || task_id.size() > 128) {
        return false;
    }
    for (char c : task_id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
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
