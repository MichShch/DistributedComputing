#include "worker_app.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "common/env.hpp"
#include "common/logging.hpp"
#include "common/time_utils.hpp"

namespace dc {
namespace worker {

namespace {

std::string DefaultOs() {
#if defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

int DefaultCpuCores() {
    unsigned int cores = std::thread::hardware_concurrency();
    return cores > 0 ? static_cast<int>(cores) : 1;
}

bool ParseBoolEnv(const std::string& value, bool fallback) {
    if (value.empty()) {
        return fallback;
    }
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool ReadFileWithLimit(const std::string& path,
                       std::size_t max_bytes,
                       std::string* data,
                       std::string* error) {
    if (!data) {
        return false;
    }
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) {
            *error = "stat failed for " + path + ": " + ec.message();
        }
        return false;
    }
    if (size > max_bytes) {
        if (error) {
            *error = "log too large (" + std::to_string(size) + " bytes)";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error) {
            *error = "open failed for " + path;
        }
        return false;
    }
    data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::optional<std::string> OptionalNonEmpty(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

WorkerConfig LoadWorkerConfigFromEnv() {
    WorkerConfig cfg;
    cfg.master_url = dc::common::GetEnvOrDefault("MASTER_URL", "http://localhost:8080");
    cfg.agent_id = dc::common::GetEnvOrDefault("AGENT_ID", "");
    cfg.os = dc::common::GetEnvOrDefault("AGENT_OS", DefaultOs());
    cfg.version = dc::common::GetEnvOrDefault("AGENT_VERSION", "dev");
    cfg.cpu_cores = dc::common::GetEnvIntOrDefault("CPU_CORES", DefaultCpuCores());
    cfg.ram_mb = dc::common::GetEnvIntOrDefault("RAM_MB", 0);
    cfg.slots = dc::common::GetEnvIntOrDefault("SLOTS", 1);
    cfg.http_timeout_ms = dc::common::GetEnvIntOrDefault("WORKER_HTTP_TIMEOUT_MS", 5000);
    cfg.log_dir = dc::common::GetEnvOrDefault("WORKER_LOG_DIR", "logs/worker");
    cfg.cancel_check_interval_sec =
        dc::common::GetEnvIntOrDefault("CANCEL_CHECK_SEC", 1);
    cfg.upload_logs = ParseBoolEnv(dc::common::GetEnvOrDefault("UPLOAD_LOGS", "true"), true);
    cfg.max_upload_bytes = static_cast<std::size_t>(
        dc::common::GetEnvIntOrDefault("MAX_UPLOAD_BYTES", 10 * 1024 * 1024));
    return cfg;
}

bool ValidateWorkerConfig(const WorkerConfig& config, std::string* error) {
    if (config.master_url.empty()) {
        if (error) {
            *error = "MASTER_URL is required";
        }
        return false;
    }
    if (config.agent_id.empty()) {
        if (error) {
            *error = "AGENT_ID is required";
        }
        return false;
    }
    if (config.cpu_cores <= 0) {
        if (error) {
            *error = "CPU_CORES must be positive";
        }
        return false;
    }
    if (config.slots <= 0) {
        if (error) {
            *error = "SLOTS must be positive";
        }
        return false;
    }
    if (config.cancel_check_interval_sec <= 0) {
        if (error) {
            *error = "CANCEL_CHECK_SEC must be positive";
        }
        return false;
    }
    return true;
}

std::unique_ptr<WorkerApp> CreateWorkerAppFromEnv(std::string* error) {
    WorkerConfig config = LoadWorkerConfigFromEnv();
    std::string validation_error;
    if (!ValidateWorkerConfig(config, &validation_error)) {
        if (error) {
            *error = "Invalid configuration: " + validation_error;
        }
        return nullptr;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.log_dir, ec);
    if (ec) {
        if (error) {
            *error = "Failed to create log dir: " + config.log_dir;
        }
        return nullptr;
    }

    const std::string log_level =
        dc::common::GetEnvOrDefault("WORKER_LOG_LEVEL", "info");
    const std::string log_file =
        dc::common::GetEnvOrDefault("WORKER_LOG_FILE", config.log_dir + "/worker.log");
    const auto resolved_level =
        dc::common::ParseLogLevel(log_level, spdlog::level::info);
    dc::common::InitLogging(log_file, resolved_level);
    spdlog::info("Worker starting (id={}, master_url={}, os={}, version={}, slots={})",
                 config.agent_id, config.master_url, config.os, config.version, config.slots);

    auto client = std::make_unique<AgentClient>(config.master_url, config.http_timeout_ms);
    return std::make_unique<WorkerApp>(std::move(config), std::move(client));
}

WorkerApp::WorkerApp(WorkerConfig config, std::unique_ptr<AgentClient> client)
    : config_(std::move(config)),
      client_(std::move(client)),
      executor_(config_.log_dir),
      workerState_(WorkerState::IDLE) {}

bool WorkerApp::Register() {
    AgentRegistration reg;
    reg.agent_id = config_.agent_id;
    reg.os = config_.os;
    reg.version = config_.version;
    reg.cpu_cores = config_.cpu_cores;
    reg.ram_mb = config_.ram_mb;
    reg.slots = config_.slots;

    HeartbeatResponse hb_resp;
    std::string error;
    if (!client_->RegisterAgent(reg, &hb_resp, &error)) {
        spdlog::error("Agent registration failed: {}", error);
        return false;
    }

    if (hb_resp.heartbeat_interval_sec > 0) {
        heartbeat_interval_sec_ = hb_resp.heartbeat_interval_sec;
    }
    spdlog::info("Agent registered: id={} os={} version={} slots={} heartbeat={}s",
                 config_.agent_id, config_.os, config_.version, config_.slots,
                 heartbeat_interval_sec_);
    return true;
}

std::string WorkerApp::translateWorkerState(const WorkerState &workerState) {
    switch(workerState) {
        case WorkerState::OFFLINE:
            return "offline";
        case WorkerState::IDLE:
            return "idle";
        case WorkerState::BUSY:
            return "busy";
        default:
            throw std::runtime_error("Unexpected case");
    }
}

std::string WorkerApp::getWorkerStateTranslated() const {
    return translateWorkerState(workerState_);
}

void WorkerApp::TickOnce() {
    workerState_ = WorkerState::IDLE;
    std::string error;
    if (!client_->SendHeartbeat(config_.agent_id, getWorkerStateTranslated(), &error)) {
        spdlog::warn("Heartbeat failed: {}", error);
    } else {
        spdlog::debug("Heartbeat sent");
    }

    std::vector<TaskDispatch> tasks;
    if (!client_->PollTasks(config_.agent_id, config_.slots, &tasks, &error)) {
        spdlog::warn("Poll tasks failed: {}", error);
        return;
    }

    if (tasks.empty()) {
        spdlog::debug("No tasks received");
        return;
    }

    for (const auto& task : tasks) {
        spdlog::info("Received task: id={} command={} args={} timeout={}s", task.task_id,
                     task.command, task.args.size(), task.timeout_sec.value_or(-1));

        if (task.constraints.contains("os") && task.constraints["os"].is_string()) {
            if (task.constraints["os"].get<std::string>() != config_.os) {
                std::string msg = "OS constraint mismatchpp";
                client_->UpdateTaskStatus(task.task_id, "failed", 1, std::nullopt,
                                          dc::common::NowUtcIso8601(), msg, &error);
                continue;
            }
        }

        workerState_ = WorkerState::BUSY;

        const auto started_at = dc::common::NowUtcIso8601();
        if (!client_->UpdateTaskStatus(task.task_id, "running", std::nullopt, started_at,
                                       std::nullopt, std::nullopt, &error)) {
            spdlog::warn("Failed to mark task {} running: {}", task.task_id, error);
        }

        auto last_cancel_check = std::chrono::steady_clock::now();
        auto cancel_interval = std::chrono::seconds(config_.cancel_check_interval_sec);
        auto is_canceled = [&]() -> bool {
            auto now = std::chrono::steady_clock::now();
            if (now - last_cancel_check < cancel_interval) {
                return false;
            }
            last_cancel_check = now;
            std::string state;
            std::string err;
            if (!client_->GetTaskState(task.task_id, &state, &err)) {
                spdlog::warn("Cancel check failed for {}: {}", task.task_id, err);
                return false;
            }
            spdlog::debug("State equals "+state);
            return state == "canceled" || state == "queued";
        };
        auto doHeartbeat = [&]() -> void {
            std::string error_task;
            if (!client_->SendHeartbeat(config_.agent_id, getWorkerStateTranslated(), &error_task)) {
                spdlog::warn("Heartbeat failed: {}", error_task);
            } else {
                spdlog::debug("Heartbeat sent");
            }
        };

        TaskExecutionResult exec = executor_.Run(task, is_canceled, doHeartbeat);
        std::string state = "failed";
        std::optional<int> exit_code = exec.exit_code;
        std::string error_message;
        if (exec.failed_to_start) {
            error_message = exec.error_message.empty() ? "Failed to start task" : exec.error_message;
        } else if (exec.timed_out) {
            error_message = "Task timed out";
        } else if (exec.canceled) {
            state = "canceled";
            error_message = "Canceled by master";
        } else if (exec.exit_code == 0) {
            state = "succeeded";
        }

        if (config_.upload_logs) {
            auto upload_stream = [&](const std::string& stream, const std::string& path) {
                std::string data;
                std::string read_error;
                if (!ReadFileWithLimit(path, config_.max_upload_bytes, &data, &read_error)) {
                    return std::string("read ") + stream + " log failed: " + read_error;
                }
                std::string http_error;
                if (!client_->UploadTaskLog(task.task_id, stream, data, &http_error)) {
                    return std::string("upload ") + stream + " log failed: " + http_error;
                }
                return std::string();
            };

            std::string up_err = upload_stream("stdout", exec.stdout_path);
            if (up_err.empty()) {
                std::string err2 = upload_stream("stderr", exec.stderr_path);
                if (!err2.empty()) {
                    up_err = std::move(err2);
                }
            }
            if (!up_err.empty()) {
                if (!error_message.empty()) {
                    error_message += "; ";
                }
                error_message += up_err;
            }
        }
        if (!exec.canceled) {
            if (!client_->UpdateTaskStatus(task.task_id,
                                        state,
                                        exit_code,
                                        OptionalNonEmpty(started_at),
                                        OptionalNonEmpty(exec.finished_at),
                                        OptionalNonEmpty(error_message),
                                        &error)) {
                spdlog::warn("Failed to update task {} status: {}", task.task_id, error);
            } else {
                spdlog::info("Task {} completed: state={} exit_code={} log_dir={}",
                            task.task_id, state, exec.exit_code, exec.stdout_path);
            }
        }
    }
}

int WorkerApp::Run(bool run_once) {
    if (!Register()) {
        return 1;
    }

    if (run_once) {
        TickOnce();
        return 0;
    }

    while (true) {
        TickOnce();
        std::this_thread::sleep_for(std::chrono::seconds(heartbeat_interval_sec_));
    }
}

}  // namespace worker
}  // namespace dc
