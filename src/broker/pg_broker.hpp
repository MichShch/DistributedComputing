#pragma once

#include "broker.hpp"
#include "status.hpp"

namespace dc {
namespace broker {

class PgBroker final : public Broker {
public:
    explicit PgBroker(DbConfig& config);   // constructor

    std::string GenerateConnectionString(
        const DbConfig& config
    ) const override;

    bool UpsertAgent(const AgentInput& agent) override;
    bool UpdateHeartbeat(const AgentHeartbeat& heartbeat) override;
    std::optional<AgentRecord> GetAgent(const std::string& agent_id) override;
    std::vector<AgentRecord> ListAgents(const std::optional<AgentStatus>& status,
                                        int limit, int offset) override;
    std::int64_t CreateTask(const TaskInput& task) override;
    std::optional<TaskRecord> GetTask(std::int64_t task_id) override;
    std::vector<TaskSummary> ListTasks(const std::optional<TaskState>& state,
                                       const std::optional<std::string>& agent_id,
                                       int limit, int offset) override;
    std::optional<std::vector<TaskDispatch>> PollTasksForAgent(const std::string& agent_id,
                                                               int free_slots) override;
    bool UpdateTaskStatus(std::int64_t task_id,
                          TaskState state,
                          const std::optional<int>& exit_code,
                          const std::optional<std::string>& started_at,
                          const std::optional<std::string>& finished_at,
                          const std::optional<std::string>& error_message) override;
    CancelTaskResult CancelTask(std::int64_t task_id) override;
    int MarkOfflineAgentsAndRequeue(int offline_after_sec) override;
};
}  // namespace broker
}  // namespace dc
