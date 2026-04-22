#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace dc {
namespace worker {

struct AgentRegistration {
    std::string agent_id;
    std::string os;
    std::string version;
    int cpu_cores = 0;
    int ram_mb = 0;
    int slots = 1;
};

struct HeartbeatResponse {
    int heartbeat_interval_sec = 0;
};

struct TaskDispatch {
    std::string task_id;
    std::string command;
    std::vector<std::string> args;
    nlohmann::json env;
    std::optional<int> timeout_sec;
    nlohmann::json constraints;
};

class AgentClient {
public:
    AgentClient(std::string base_url, const int timeout_ms);
    virtual ~AgentClient() = default;

    virtual bool RegisterAgent(const AgentRegistration& reg,
                               HeartbeatResponse* response,
                               std::string* error) const;

    virtual bool SendHeartbeat(const std::string& agent_id,
                               const std::string& status,
                               std::string* error) const;

    virtual bool PollTasks(const std::string& agent_id,
                           int free_slots,
                           std::vector<TaskDispatch>* tasks,
                           std::string* error) const;

    virtual bool GetTaskState(const std::string& task_id,
                              std::string* state,
                              std::string* error) const;

    virtual bool UpdateTaskStatus(const std::string& task_id,
                                  const std::string& state,
                                  const std::optional<int>& exit_code,
                                  const std::optional<std::string>& started_at,
                                  const std::optional<std::string>& finished_at,
                                  const std::optional<std::string>& error_message,
                                  std::string* error) const;

    virtual bool UploadTaskLog(const std::string& task_id,
                               const std::string& stream,
                               const std::string& data,
                               std::string* error) const;

private:
    httplib::Headers DefaultHeaders() const;
    bool IsSuccess(const httplib::Result& res, std::string* error) const;

    mutable std::unique_ptr<httplib::Client> client_;
};

}  // namespace worker
}  // namespace dc
