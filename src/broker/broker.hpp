#pragma once

#include <nlohmann/json.hpp>

#include "common/logging.hpp"
#include "status.hpp"

namespace dc {
namespace broker { 

enum class AuthentificationMethod {
    PASSWORD,
    SSL,
};

AuthentificationMethod getAuthMethod(const std::string& method);

struct SSLCerificates {
    std::string rootcert;
    std::string cert;
    std::string key;
};

struct DbConfig {
    std::string host;
    std::string port;
    std::string user;
    std::string password;
    std::string dbname;
    std::string mongo_auth_source;
    AuthentificationMethod authMethod;
    SSLCerificates ssl;
    int reconnect_attempts = 5;
    int reconnect_cooldown_sec = 2;
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
    AgentStatus status = AgentStatus::Idle;
};

struct AgentRecord {
    std::string agent_id;
    std::string os;
    std::string version;
    int cpu_cores = 0;
    int ram_mb = 0;
    int slots = 0;
    AgentStatus status = AgentStatus::Idle;
    std::string last_heartbeat;
};

struct TaskInput {
    std::string command;
    nlohmann::json args;
    nlohmann::json env;
    std::optional<int> timeout_sec;
    nlohmann::json constraints;
};

struct TaskRecord {
    std::int64_t task_id = 0;
    TaskState state = TaskState::Queued;
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
    std::int64_t task_id = 0;
    TaskState state = TaskState::Queued;
};

struct TaskDispatch {
    std::int64_t task_id = 0;
    std::string command;
    nlohmann::json args;
    nlohmann::json env;
    std::optional<int> timeout_sec;
    nlohmann::json constraints;
};

enum class CancelTaskResult {
    Ok,
    NotFound,
    InvalidState,
    Error,
};

enum class BrokerType {
    PGSQL,
    MONGO,
};

class Broker {
public:
    explicit Broker(DbConfig& config, BrokerType brokerType, std::string connectionString): config_(config), broker_type_(brokerType), connectionString_(connectionString){};

    virtual ~Broker() = default;

    virtual std::string GenerateConnectionString(
        const DbConfig& config
    ) const = 0;

    virtual bool UpsertAgent(const AgentInput& agent) = 0;
    virtual bool UpdateHeartbeat(const AgentHeartbeat& heartbeat) = 0;

    virtual std::optional<AgentRecord> GetAgent(const std::string& agent_id) = 0;
    virtual std::vector<AgentRecord> ListAgents(const std::optional<AgentStatus>& status,
                                        int limit,
                                        int offset) = 0;

    virtual std::int64_t CreateTask(const TaskInput& task) = 0;
    virtual std::optional<TaskRecord> GetTask(std::int64_t task_id) = 0;
    virtual std::vector<TaskSummary> ListTasks(const std::optional<TaskState>& state,
                                       const std::optional<std::string>& agent_id,
                                       int limit,
                                       int offset) = 0;

    // Returns std::nullopt when agent does not exist.
    virtual std::optional<std::vector<TaskDispatch>> PollTasksForAgent(const std::string& agent_id,
                                                               int free_slots) = 0;

    virtual bool UpdateTaskStatus(std::int64_t task_id,
                          TaskState state,
                          const std::optional<int>& exit_code,
                          const std::optional<std::string>& started_at,
                          const std::optional<std::string>& finished_at,
                          const std::optional<std::string>& error_message) = 0;

    virtual CancelTaskResult CancelTask(std::int64_t task_id) = 0;

    // Marks agents offline and requeues tasks assigned to them.
    virtual int MarkOfflineAgentsAndRequeue(int offline_after_sec) = 0;

    const BrokerType GetBrokerType() const;

    const std::string GetConnectionString() const;

    const DbConfig GetDbConfig() const;

protected:
    template <typename Fn>
    auto ExecuteWithRetry(const std::string& operation, Fn&& fn) const -> decltype(fn()) {
        const int attempts = config_.reconnect_attempts;
        const int cooldown_sec = config_.reconnect_cooldown_sec;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            try {
                return fn();
            } catch (const std::exception& ex) {
                if (attempt >= attempts) {
                    spdlog::error(
                        "Broker operation '{}' failed on final attempt {}/{}: {}",
                        operation,
                        attempt,
                        attempts,
                        ex.what());
                    throw;
                }
                spdlog::warn(
                    "Broker operation '{}' failed on attempt {}/{}: {}. Retrying in {}s.",
                    operation,
                    attempt,
                    attempts,
                    ex.what(),
                    cooldown_sec);
            } catch (...) {
                if (attempt >= attempts) {
                    spdlog::error(
                        "Broker operation '{}' failed on final attempt {}/{} with unknown exception.",
                        operation,
                        attempt,
                        attempts);
                    throw;
                }
                spdlog::warn(
                    "Broker operation '{}' failed on attempt {}/{} with unknown exception. Retrying in {}s.",
                    operation,
                    attempt,
                    attempts,
                    cooldown_sec);
            }

            if (cooldown_sec > 0) {
                std::this_thread::sleep_for(std::chrono::seconds(cooldown_sec));
            }
        }

        throw std::runtime_error("Retry loop exhausted unexpectedly");
    }

    const std::string connectionString_;

    const BrokerType broker_type_;
    const DbConfig config_;
};

}  // namespace broker
}  // namespace dc
