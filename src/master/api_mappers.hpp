#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "broker/broker.hpp"

namespace dc {
using namespace broker;
namespace master {
namespace api {

struct TaskStatusUpdate {
    TaskState state = TaskState::Queued;
    std::optional<int> exit_code;
    std::optional<std::string> started_at;
    std::optional<std::string> finished_at;
    std::optional<std::string> error_message;
};

bool ParseAgentInput(const std::string& agent_id,
                     const nlohmann::json& body,
                     AgentInput* out,
                     std::string* error);

bool ParseAgentHeartbeat(const std::string& agent_id,
                         const nlohmann::json& body,
                         AgentHeartbeat* out,
                         std::string* error);

bool ParseTaskCreate(const nlohmann::json& body, TaskInput* out, std::string* error);

bool ParseTaskStatusUpdate(const nlohmann::json& body,
                           TaskStatusUpdate* out,
                           std::string* error);

nlohmann::json AgentRecordToJson(const AgentRecord& agent);
nlohmann::json AgentSummaryToJson(const AgentRecord& agent);
nlohmann::json TaskRecordToJson(const TaskRecord& task);
nlohmann::json TaskSummaryToJson(const TaskSummary& task);
nlohmann::json TaskDispatchToJson(const TaskDispatch& task);

std::optional<std::int64_t> ParseTaskId(const std::string& task_id);
bool IsValidTaskStateTransition(TaskState from, TaskState to);

}  // namespace api
}  // namespace master
}  // namespace dc
