#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent_client.hpp"
#include "task_executor.hpp"

namespace dc {
namespace worker {

enum class WorkerState{
    OFFLINE,
    IDLE,
    BUSY,
};

struct WorkerConfig {
    std::string master_url;
    std::string agent_id;
    std::string os;
    std::string version;
    int cpu_cores = 0;
    int ram_mb = 0;
    int slots = 1;
    int http_timeout_ms = 5000;
    std::string log_dir;
    int cancel_check_interval_sec = 1;
    bool upload_logs = true;
    std::size_t max_upload_bytes = 10 * 1024 * 1024;  // 10MB
};

WorkerConfig LoadWorkerConfigFromEnv();
bool ValidateWorkerConfig(const WorkerConfig& config, std::string* error);

class WorkerApp {
public:
    WorkerApp(WorkerConfig config, std::unique_ptr<AgentClient> client);

    // Runs the agent; when run_once is true, performs a single heartbeat+poll cycle.
    int Run(bool run_once);

    std::string getWorkerStateTranslated() const;

private:
    bool Register();
    void TickOnce();
    static std::string translateWorkerState(const WorkerState &workerState);

    WorkerConfig config_;
    std::unique_ptr<AgentClient> client_;
    int heartbeat_interval_sec_ = 10;
    TaskExecutor executor_;
    WorkerState workerState_;
};

// Собирает WorkerApp: читает конфигурацию из окружения, валидирует, создаёт клиент.
std::unique_ptr<WorkerApp> CreateWorkerAppFromEnv(std::string* error);

}  // namespace worker
}  // namespace dc
