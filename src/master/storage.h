#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dc {
namespace master {

struct DbConfig {
    std::string host;
    std::string port;
    std::string user;
    std::string password;
    std::string dbname;
    std::string sslmode;
};

struct AgentInput {
    std::string agent_id;
    std::string os;
    std::string version;
    int cpu_cores = 0;
    int ram_mb = 0;
    int slots = 0;
};

struct AgentHeartbeat {
    std::string agent_id;
    std::string status;
};

struct AgentRecord {
    std::string agent_id;
    std::string os;
    std::string version;
    int cpu_cores = 0;
    int ram_mb = 0;
    int slots = 0;
    std::string status;
    std::string last_heartbeat;
};

struct TaskInput {
    std::string task_id;
    std::string command;
    nlohmann::json args;
    nlohmann::json env;
    std::optional<int> timeout_sec;
    nlohmann::json constraints;
};

struct TaskRecord {
    std::string task_id;
    std::string state;
    std::string command;
    nlohmann::json args;
    nlohmann::json env;
    std::optional<int> timeout_sec;
    std::optional<std::string> assigned_agent;
    std::string created_at;
    std::optional<std::string> started_at;
    std::optional<std::string> finished_at;
    std::optional<int> exit_code;
    std::optional<std::string> error_message;
    nlohmann::json constraints;
};

struct TaskSummary {
    std::string task_id;
    std::string state;
};

struct TaskDispatch {
    std::string task_id;
    std::string command;
    nlohmann::json args;
    nlohmann::json env;
    std::optional<int> timeout_sec;
    nlohmann::json constraints;
};

enum class CreateTaskResult {
    Ok,
    AlreadyExists,
    Error,
};

enum class CancelTaskResult {
    Ok,
    NotFound,
    InvalidState,
    Error,
};

class Storage {
public:
    explicit Storage(DbConfig config);

    bool UpsertAgent(const AgentInput& agent);
    bool UpdateHeartbeat(const AgentHeartbeat& heartbeat);

    std::optional<AgentRecord> GetAgent(const std::string& agent_id);
    std::vector<AgentRecord> ListAgents(const std::optional<std::string>& status,
                                        int limit,
                                        int offset);

    CreateTaskResult CreateTask(const TaskInput& task);
    std::optional<TaskRecord> GetTask(const std::string& task_id);
    std::vector<TaskSummary> ListTasks(const std::optional<std::string>& state,
                                       const std::optional<std::string>& agent_id,
                                       int limit,
                                       int offset);

    // Returns std::nullopt when agent does not exist.
    std::optional<std::vector<TaskDispatch>> PollTasksForAgent(const std::string& agent_id,
                                                               int free_slots);

    bool UpdateTaskStatus(const std::string& task_id,
                          const std::string& state,
                          const std::optional<int>& exit_code,
                          const std::optional<std::string>& started_at,
                          const std::optional<std::string>& finished_at,
                          const std::optional<std::string>& error_message);

    CancelTaskResult CancelTask(const std::string& task_id);

    // Marks agents offline and requeues tasks assigned to them.
    int MarkOfflineAgentsAndRequeue(int offline_after_sec);

private:
    std::string ConnectionString() const;
    nlohmann::json SafeParseJson(const std::string& raw, const nlohmann::json& fallback) const;
    std::string DbAgentStatusFromApi(const std::string& status) const;
    std::string DbTaskStateFromApi(const std::string& state) const;
    std::string ApiAgentStatusFromDb(const std::string& status) const;
    std::string ApiTaskStateFromDb(const std::string& state) const;

    DbConfig config_;
};

}  // namespace master
}  // namespace dc
