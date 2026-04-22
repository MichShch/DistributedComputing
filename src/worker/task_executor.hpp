#pragma once

#include <optional>
#include <string>
#include <functional>

#include "agent_client.hpp"

namespace dc {
namespace worker {

struct TaskExecutionResult {
    int exit_code = -1;
    bool timed_out = false;
    bool failed_to_start = false;
    bool canceled = false;
    std::string error_message;
    std::string started_at;
    std::string finished_at;
    std::string stdout_path;
    std::string stderr_path;
};

class TaskExecutor {
public:
    explicit TaskExecutor(std::string log_root);

    TaskExecutionResult Run(const TaskDispatch& task,
                            const std::function<bool()>& is_canceled,
                            const std::function<void()>& doHeartbeat);

private:
    std::string EnsureTaskLogDir(const std::string& task_id);

    std::string log_root_;
};

}  // namespace worker
}  // namespace dc
