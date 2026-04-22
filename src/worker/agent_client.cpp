#include "agent_client.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>

#include "common/string_utils.hpp"

namespace dc {
namespace worker {

namespace {

std::string BuildAgentPath(const std::string& agent_id, const std::string& suffix) {
    return "/api/v1/agents/" + agent_id + suffix;
}

std::string BuildTaskPath(const std::string& task_id, const std::string& suffix) {
    return "/api/v1/tasks/" + task_id + suffix;
}

}  // namespace

AgentClient::AgentClient(std::string base_url, const int timeout_ms)
    : client_(std::make_unique<httplib::Client>(std::move(base_url))) {
    if (timeout_ms > 0) {
        auto timeout = std::chrono::milliseconds(timeout_ms);
        client_->set_connection_timeout(timeout);
        client_->set_read_timeout(timeout);
        client_->set_write_timeout(timeout);
    }
}

httplib::Headers AgentClient::DefaultHeaders() const {
    httplib::Headers headers;
    headers.emplace("Accept", "application/json");
    headers.emplace("Content-Type", "application/json");
    return headers;
}

bool AgentClient::IsSuccess(const httplib::Result& res, std::string* error) const {
    if (!res) {
        if (error) {
            *error = "Request failed";
        }
        return false;
    }
    if (res->status >= 200 && res->status < 300) {
        return true;
    }
    if (error) {
        std::ostringstream oss;
        oss << "Unexpected status: " << res->status << " body=" << res->body;
        *error = oss.str();
    }
    return false;
}

bool AgentClient::RegisterAgent(const AgentRegistration& reg,
                                HeartbeatResponse* response,
                                std::string* error) const {
    nlohmann::json payload{
        {"os", reg.os},
        {"version", reg.version},
        {"resources",
         {
             {"cpu_cores", reg.cpu_cores},
             {"ram_mb", reg.ram_mb},
             {"slots", reg.slots},
         }},
    };

    auto res = client_->Put(BuildAgentPath(reg.agent_id, "").c_str(), DefaultHeaders(),
                            payload.dump(), "application/json");
    if (!IsSuccess(res, error)) {
        return false;
    }

    if (response) {
        try {
            auto body = nlohmann::json::parse(res->body);
            if (body.contains("heartbeat_interval_sec") &&
                body["heartbeat_interval_sec"].is_number_integer()) {
                response->heartbeat_interval_sec = body["heartbeat_interval_sec"].get<int>();
            }
        } catch (const std::exception& ex) {
            if (error) {
                *error = std::string("Failed to parse register response: ") + ex.what();
            }
            return false;
        }
    }
    return true;
}

bool AgentClient::SendHeartbeat(const std::string& agent_id,
                                const std::string& status,
                                std::string* error) const {
    nlohmann::json payload{
        {"status", status},
    };

    auto res = client_->Post(BuildAgentPath(agent_id, "/heartbeat").c_str(), DefaultHeaders(),
                             payload.dump(), "application/json");
    return IsSuccess(res, error);
}

bool AgentClient::PollTasks(const std::string& agent_id,
                            int free_slots,
                            std::vector<TaskDispatch>* tasks,
                            std::string* error) const {
    nlohmann::json payload{
        {"free_slots", free_slots},
    };

    auto res = client_->Post(BuildAgentPath(agent_id, "/tasks:poll").c_str(), DefaultHeaders(),
                             payload.dump(), "application/json");
    if (!IsSuccess(res, error)) {
        return false;
    }

    if (!tasks) {
        return true;
    }

    try {
        auto body = nlohmann::json::parse(res->body);
        if (!body.contains("tasks") || !body["tasks"].is_array()) {
            if (error) {
                *error = "Invalid response: tasks array missing";
            }
            return false;
        }
        tasks->clear();
        for (const auto& item : body["tasks"]) {
            TaskDispatch dispatch;
            if (!item.contains("task_id") || !item.contains("command")) {
                continue;
            }
            if (item["task_id"].is_number_integer()) {
                dispatch.task_id = std::to_string(item["task_id"].get<std::int64_t>());
            } else {
                continue;
            }
            dispatch.command = item["command"].get<std::string>();
            if (item.contains("args") && item["args"].is_array()) {
                for (const auto& arg : item["args"]) {
                    if (arg.is_string()) {
                        dispatch.args.push_back(arg.get<std::string>());
                    }
                }
            }
            if (item.contains("env") && item["env"].is_object()) {
                dispatch.env = item["env"];
            } else {
                dispatch.env = nlohmann::json::object();
            }
            if (item.contains("timeout_sec") && item["timeout_sec"].is_number_integer()) {
                dispatch.timeout_sec = item["timeout_sec"].get<int>();
            }
            if (item.contains("constraints") && item["constraints"].is_object()) {
                dispatch.constraints = item["constraints"];
            } else {
                dispatch.constraints = nlohmann::json::object();
            }
            tasks->push_back(std::move(dispatch));
        }
    } catch (const std::exception& ex) {
        if (error) {
            *error = std::string("Failed to parse tasks: ") + ex.what();
        }
        return false;
    }

    return true;
}

bool AgentClient::UpdateTaskStatus(const std::string& task_id,
                                   const std::string& state,
                                   const std::optional<int>& exit_code,
                                   const std::optional<std::string>& started_at,
                                   const std::optional<std::string>& finished_at,
                                   const std::optional<std::string>& error_message,
                                   std::string* error) const {
    nlohmann::json payload{
        {"state", state},
    };
    if (exit_code) {
        payload["exit_code"] = *exit_code;
    }
    if (started_at) {
        payload["started_at"] = *started_at;
    }
    if (finished_at) {
        payload["finished_at"] = *finished_at;
    }
    if (error_message) {
        payload["error_message"] = dc::common::SanitizeUtf8Lossy(*error_message);
    }

    auto res = client_->Post(BuildTaskPath(task_id, "/status").c_str(), DefaultHeaders(),
                             payload.dump(), "application/json");
    return IsSuccess(res, error);
}

bool AgentClient::UploadTaskLog(const std::string& task_id,
                                const std::string& stream,
                                const std::string& data,
                                std::string* error) const {
    const std::string safe_data = dc::common::SanitizeUtf8Lossy(data);
    nlohmann::json payload{
        {"stream", stream},
        {"data", safe_data},
    };
    auto res = client_->Post(BuildTaskPath(task_id, "/logs:upload").c_str(),
                             DefaultHeaders(),
                             payload.dump(),
                             "application/json");
    return IsSuccess(res, error);
}

bool AgentClient::GetTaskState(const std::string& task_id,
                               std::string* state,
                               std::string* error) const {
    auto res = client_->Get(BuildTaskPath(task_id, "").c_str(), {}, DefaultHeaders());
    if (!IsSuccess(res, error)) {
        return false;
    }
    try {
        auto body = nlohmann::json::parse(res->body);
        if (body.contains("task") && body["task"].is_object() && body["task"].contains("state")) {
            if (state) {
                *state = body["task"]["state"].get<std::string>();
            }
            return true;
        }
    } catch (const std::exception& ex) {
        if (error) {
            *error = std::string("Failed to parse task state: ") + ex.what();
        }
        return false;
    }
    if (error) {
        *error = "Invalid task state response";
    }
    return false;
}

}  // namespace worker
}  // namespace dc
