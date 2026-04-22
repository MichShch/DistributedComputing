#include "control_service.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/logging.hpp"
#include "api_mappers.hpp"
namespace dc {
namespace master {

using json = nlohmann::json;

namespace {

json MakeError(const std::string& code,
               const std::string& message,
               const json& details = json::object()) {
    json error;
    error["error"]["code"] = code;
    error["error"]["message"] = message;
    error["error"]["details"] = details;
    return error;
}

void SetJsonResponse(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(2), "application/json; charset=utf-8");
}

bool ParseJsonBody(const httplib::Request& req, json* out, std::string* error) {
    if (!out) {
        return false;
    }
    try {
        if (req.body.empty()) {
            *out = json::object();
            return true;
        }
        *out = json::parse(req.body);
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
}

std::optional<std::string> GetQueryParam(const httplib::Request& req,
                                         const std::string& name) {
    auto it = req.params.find(name);
    if (it == req.params.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::int64_t> ParseTaskIdParam(const std::string& raw) {
    return api::ParseTaskId(raw);
}

int GetQueryParamInt(const httplib::Request& req, const std::string& name, int fallback) {
    auto value = GetQueryParam(req, name);
    if (!value) {
        return fallback;
    }
    try {
        return std::stoi(*value);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

ControlService::ControlService(MasterConfig config, Broker* broker, LogStore log_store)
    : config_(config),
      broker_(broker),
      log_store_(log_store) {}

ControlService::~ControlService() = default;

void ControlService::RegisterRoutes() {
    // All endpoints live under /api/v1 to keep versioned compatibility with future changes.
    server_->Put(R"(/api/v1/agents/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleUpsertAgent(req, res);
    });

    server_->Post(R"(/api/v1/agents/([^/]+)/heartbeat)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        HandleAgentHeartbeat(req, res);
    });

    server_->Post(R"(/api/v1/agents/([^/]+)/tasks:poll)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        HandlePollTasks(req, res);
    });

    server_->Post(R"(/api/v1/tasks)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        HandleCreateTask(req, res);
    });

    server_->Get(R"(/api/v1/tasks/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetTask(req, res);
    });

    server_->Get(R"(/api/v1/tasks)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleListTasks(req, res);
    });

    server_->Post(R"(/api/v1/tasks/([^/]+)/status)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        HandleUpdateTaskStatus(req, res);
    });

    server_->Post(R"(/api/v1/tasks/([^/]+)/cancel)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        HandleCancelTask(req, res);
    });

    server_->Get(R"(/api/v1/agents/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetAgent(req, res);
    });

    server_->Get(R"(/api/v1/agents)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleListAgents(req, res);
    });

    // Logs are served from file broker; metadata is updated on each read.
    server_->Get(R"(/api/v1/tasks/([^/]+)/logs)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetLogs(req, res);
    });

    server_->Get(R"(/api/v1/tasks/([^/]+)/logs:tail)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        HandleTailLogs(req, res);
    });

    server_->Post(R"(/api/v1/tasks/([^/]+)/logs:upload)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        HandleUploadLogs(req, res);
    });
}

void ControlService::HandleUpsertAgent(const httplib::Request& req,
                                       httplib::Response& res) {
    const std::string agent_id = req.matches[1];
    json body;
    std::string error;
    if (!ParseJsonBody(req, &body, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
        return;
    }

    AgentInput agent;
    if (!api::ParseAgentInput(agent_id, body, &agent, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", error), 400);
        return;
    }

    try {
        broker_->UpsertAgent(agent);
    } catch (const std::exception& ex) {
        spdlog::error("DB error upserting agent {}: {}", agent_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    spdlog::info("Agent upserted: {}", agent_id);
    json response;
    response["status"] = "ok";
    response["heartbeat_interval_sec"] = config_.heartbeat_interval_sec;
    SetJsonResponse(res, response, 200);
}

void ControlService::HandleAgentHeartbeat(const httplib::Request& req,
                                          httplib::Response& res) {
    const std::string agent_id = req.matches[1];
    json body;
    std::string error;
    if (!ParseJsonBody(req, &body, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
        return;
    }

    AgentHeartbeat heartbeat;
    if (!api::ParseAgentHeartbeat(agent_id, body, &heartbeat, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", error), 400);
        return;
    }

    try {
        if (!broker_->UpdateHeartbeat(heartbeat)) {
            SetJsonResponse(res, MakeError("AGENT_NOT_FOUND", "Agent not found",
                                           {{"agent_id", agent_id}}), 404);
            return;
        }
    } catch (const std::exception& ex) {
        spdlog::error("DB error updating heartbeat for {}: {}", agent_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    spdlog::debug("Heartbeat updated: {} status={}", agent_id, AgentStatusToApi(heartbeat.status));
    SetJsonResponse(res, json{{"status", "ok"}}, 200);
}

void ControlService::HandlePollTasks(const httplib::Request& req,
                                     httplib::Response& res) {
    const std::string agent_id = req.matches[1];
    json body;
    std::string error;
    if (!ParseJsonBody(req, &body, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
        return;
    }

    if (!body.contains("free_slots") || !body["free_slots"].is_number_integer()) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing free_slots"), 400);
        return;
    }

    int free_slots = body["free_slots"].get<int>();

    std::optional<std::vector<TaskDispatch>> dispatches;
    try {
        dispatches = broker_->PollTasksForAgent(agent_id, free_slots);
    } catch (const std::exception& ex) {
        spdlog::error("DB error polling tasks for {}: {}", agent_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    if (!dispatches) {
        SetJsonResponse(res, MakeError("AGENT_NOT_FOUND", "Agent not found",
                                       {{"agent_id", agent_id}}), 404);
        return;
    }

    spdlog::debug("Tasks polled for {}: {} dispatches", agent_id, dispatches->size());
    json response;
    response["tasks"] = json::array();
    for (const auto& task : *dispatches) {
        response["tasks"].push_back(api::TaskDispatchToJson(task));
    }
    SetJsonResponse(res, response, 200);
}

void ControlService::HandleCreateTask(const httplib::Request& req,
                                      httplib::Response& res) {
    json body;
    std::string error;
    if (!ParseJsonBody(req, &body, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
        return;
    }

    TaskInput task;
    if (!api::ParseTaskCreate(body, &task, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", error), 400);
        return;
    }

    std::int64_t created_task_id = 0;
    try {
        created_task_id = broker_->CreateTask(task);
    } catch (const std::exception& ex) {
        spdlog::error("DB error creating task: {}", ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }
    if (created_task_id <= 0) {
        spdlog::error("DB error creating task: broker returned invalid task id {}", created_task_id);
        SetJsonResponse(res, MakeError("DB_ERROR", "Failed to create task"), 500);
        return;
    }

    spdlog::info("Task created: {}", created_task_id);
    json response;
    response["task_id"] = created_task_id;
    SetJsonResponse(res, response, 201);
}

void ControlService::HandleGetTask(const httplib::Request& req,
                                   httplib::Response& res) {
    const std::string task_id_raw = req.matches[1];
    auto task_id = ParseTaskIdParam(task_id_raw);
    if (!task_id) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
        return;
    }
    std::optional<TaskRecord> task;
    try {
        task = broker_->GetTask(*task_id);
    } catch (const std::exception& ex) {
        spdlog::error("DB error fetching task {}: {}", *task_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    if (!task) {
        SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                       {{"task_id", *task_id}}), 404);
        return;
    }

    SetJsonResponse(res, json{{"task", api::TaskRecordToJson(*task)}}, 200);
}

void ControlService::HandleListTasks(const httplib::Request& req,
                                     httplib::Response& res) {
    auto state_param = GetQueryParam(req, "state");
    std::optional<TaskState> state;
    if (state_param) {
        auto parsed = TaskStateFromApi(*state_param);
        if (!parsed) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task state"), 400);
            return;
        }
        state = *parsed;
    }

    auto agent_id = GetQueryParam(req, "agent_id");
    int limit = GetQueryParamInt(req, "limit", 50);
    int offset = GetQueryParamInt(req, "offset", 0);

    std::vector<TaskSummary> tasks;
    try {
        tasks = broker_->ListTasks(state, agent_id, limit, offset);
    } catch (const std::exception& ex) {
        spdlog::error("DB error listing tasks: {}", ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    std::string state_label = state ? std::string(TaskStateToApi(*state)) : "";
    spdlog::debug("List tasks: state={} agent_id={} limit={} offset={} -> {}",
                  state_label,
                  agent_id.value_or(""),
                  limit,
                  offset,
                  tasks.size());
    json response;
    response["tasks"] = json::array();
    for (const auto& task : tasks) {
        response["tasks"].push_back(api::TaskSummaryToJson(task));
    }
    SetJsonResponse(res, response, 200);
}

void ControlService::HandleUpdateTaskStatus(const httplib::Request& req,
                                            httplib::Response& res) {
    const std::string task_id_raw = req.matches[1];
    auto task_id = ParseTaskIdParam(task_id_raw);
    if (!task_id) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
        return;
    }
    json body;
    std::string error;
    if (!ParseJsonBody(req, &body, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
        return;
    }

    api::TaskStatusUpdate update;
    if (!api::ParseTaskStatusUpdate(body, &update, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", error), 400);
        return;
    }

    std::optional<TaskRecord> current;
    try {
        current = broker_->GetTask(*task_id);
    } catch (const std::exception& ex) {
        spdlog::error("DB error fetching task {} for update: {}", *task_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }
    if (!current) {
        SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                       {{"task_id", *task_id}}), 404);
        return;
    }
    if (!api::IsValidTaskStateTransition(current->state, update.state)) {
        SetJsonResponse(res, MakeError("INVALID_STATE_TRANSITION",
                                       "Invalid task state transition",
                                       {{"from", TaskStateToApi(current->state)},
                                        {"to", TaskStateToApi(update.state)}}),
                        409);
        return;
    }

    try {
        if (!broker_->UpdateTaskStatus(*task_id,
                                       update.state,
                                       update.exit_code,
                                       update.started_at,
                                       update.finished_at,
                                       update.error_message)) {
            SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                           {{"task_id", *task_id}}), 404);
            return;
        }
    } catch (const std::exception& ex) {
        spdlog::error("DB error updating task {}: {}", *task_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    spdlog::info("Task status updated: {} {} -> {}",
                 *task_id,
                 TaskStateToApi(current->state),
                 TaskStateToApi(update.state));
    SetJsonResponse(res, json{{"status", "ok"}}, 200);
}

void ControlService::HandleCancelTask(const httplib::Request& req,
                                      httplib::Response& res) {
    const std::string task_id_raw = req.matches[1];
    auto task_id = ParseTaskIdParam(task_id_raw);
    if (!task_id) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
        return;
    }
    CancelTaskResult result;
    try {
        result = broker_->CancelTask(*task_id);
    } catch (const std::exception& ex) {
        spdlog::error("DB error canceling task {}: {}", *task_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    if (result == CancelTaskResult::NotFound) {
        SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                       {{"task_id", *task_id}}), 404);
        return;
    }
    if (result == CancelTaskResult::InvalidState) {
        SetJsonResponse(res, MakeError("TASK_INVALID_STATE",
                                       "Task already finished; cannot cancel",
                                       {{"task_id", *task_id}}),
                        409);
        return;
    }
    if (result != CancelTaskResult::Ok) {
        SetJsonResponse(res, MakeError("DB_ERROR", "Failed to cancel task"), 500);
        return;
    }

    spdlog::info("Task canceled: {}", *task_id);
    SetJsonResponse(res, json{{"status", "ok"}}, 200);
}

void ControlService::HandleGetAgent(const httplib::Request& req,
                                    httplib::Response& res) {
    const std::string agent_id = req.matches[1];
    std::optional<AgentRecord> agent;
    try {
        agent = broker_->GetAgent(agent_id);
    } catch (const std::exception& ex) {
        spdlog::error("DB error fetching agent {}: {}", agent_id, ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    if (!agent) {
        SetJsonResponse(res, MakeError("AGENT_NOT_FOUND", "Agent not found",
                                       {{"agent_id", agent_id}}), 404);
        return;
    }

    SetJsonResponse(res, json{{"agent", api::AgentRecordToJson(*agent)}}, 200);
}

void ControlService::HandleListAgents(const httplib::Request& req,
                                      httplib::Response& res) {
    auto status_param = GetQueryParam(req, "status");
    std::optional<AgentStatus> status;
    if (status_param) {
        auto parsed = AgentStatusFromApi(*status_param);
        if (!parsed) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid agent status"), 400);
            return;
        }
        status = *parsed;
    }
    int limit = GetQueryParamInt(req, "limit", 50);
    int offset = GetQueryParamInt(req, "offset", 0);

    std::vector<AgentRecord> agents;
    try {
        agents = broker_->ListAgents(status, limit, offset);
    } catch (const std::exception& ex) {
        spdlog::error("DB error listing agents: {}", ex.what());
        SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
        return;
    }

    std::string status_label = status ? std::string(AgentStatusToApi(*status)) : "";
    spdlog::debug("List agents: status={} limit={} offset={} -> {}",
                  status_label,
                  limit,
                  offset,
                  agents.size());
    json response;
    response["agents"] = json::array();
    for (const auto& agent : agents) {
        response["agents"].push_back(api::AgentSummaryToJson(agent));
    }
    SetJsonResponse(res, response, 200);
}

void ControlService::HandleGetLogs(const httplib::Request& req,
                                   httplib::Response& res) {
    const std::string task_id_raw = req.matches[1];
    auto task_id = ParseTaskIdParam(task_id_raw);
    if (!task_id) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
        return;
    }
    std::string stream = GetQueryParam(req, "stream").value_or("stdout");
    if (stream != "stdout" && stream != "stderr") {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid stream"), 400);
        return;
    }

    auto log_result = log_store_.ReadAll(std::to_string(*task_id), stream);
    if (!log_result.exists) {
        SetJsonResponse(res, MakeError("LOG_NOT_FOUND", "Log not found",
                                       {{"task_id", *task_id}, {"stream", stream}}), 404);
        return;
    }

    res.status = 200;
    res.set_content(log_result.data, "text/plain; charset=utf-8");
}

void ControlService::HandleTailLogs(const httplib::Request& req,
                                    httplib::Response& res) {
    const std::string task_id_raw = req.matches[1];
    auto task_id = ParseTaskIdParam(task_id_raw);
    if (!task_id) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
        return;
    }
    std::string stream = GetQueryParam(req, "stream").value_or("stdout");
    if (stream != "stdout" && stream != "stderr") {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid stream"), 400);
        return;
    }

    std::uint64_t offset = 0;
    if (auto from = GetQueryParam(req, "from")) {
        try {
            offset = static_cast<std::uint64_t>(std::stoull(*from));
        } catch (...) {
            offset = 0;
        }
    }

    auto log_result = log_store_.ReadFromOffset(std::to_string(*task_id), stream, offset);
    if (!log_result.exists) {
        SetJsonResponse(res, MakeError("LOG_NOT_FOUND", "Log not found",
                                       {{"task_id", *task_id}, {"stream", stream}}), 404);
        return;
    }

    res.status = 200;
    res.set_header("X-Log-Size", std::to_string(log_result.size_bytes));
    res.set_content(log_result.data, "text/plain; charset=utf-8");
}

void ControlService::HandleUploadLogs(const httplib::Request& req,
                                      httplib::Response& res) {
    const std::string task_id_raw = req.matches[1];
    auto task_id = ParseTaskIdParam(task_id_raw);
    if (!task_id) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
        return;
    }

    json body;
    std::string error;
    if (!ParseJsonBody(req, &body, &error)) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
        return;
    }

    std::string stream = body.value("stream", "stdout");
    if (stream != "stdout" && stream != "stderr") {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid stream"), 400);
        return;
    }
    if (!body.contains("data") || !body["data"].is_string()) {
        SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing data"), 400);
        return;
    }

    const std::string data = body["data"].get<std::string>();
    if (data.size() > config_.max_log_upload_bytes) {
        SetJsonResponse(res,
                        MakeError("PAYLOAD_TOO_LARGE",
                                  "Log upload exceeds limit",
                                  {{"limit_bytes", config_.max_log_upload_bytes}}),
                        413);
        return;
    }

    if (!log_store_.WriteAll(std::to_string(*task_id), stream, data)) {
        SetJsonResponse(res, MakeError("LOG_WRITE_FAILED", "Failed to store log"), 500);
        return;
    }

    json response;
    response["status"] = "ok";
    response["size_bytes"] = data.size();
    SetJsonResponse(res, response, 200);
}


void ControlService::HandleHealth(const httplib::Request& req, httplib::Response& res){
    res.status = 200;
}

void ControlService::StartMaintenanceThread() {
    running_.store(true);
    maintenance_thread_ = std::thread([this]() {
        while (running_.load()) {
            try {
                int count = broker_->MarkOfflineAgentsAndRequeue(config_.offline_after_sec);
                if (count > 0) {
                    spdlog::info("Marked {} agents offline and requeued tasks", count);
                }
            } catch (const std::exception& ex) {
                spdlog::error("Maintenance error: {}", ex.what());
            }
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });
}

void ControlService::StopMaintenanceThread() {
    running_.store(false);
    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }
}

int ControlService::Run() {
    server_ = std::make_unique<httplib::Server>();
    server_->set_logger([](const httplib::Request& req, const httplib::Response& res) {
        spdlog::debug("HTTP {} {} -> {}", req.method, req.path, res.status);
    });

    RegisterRoutes();

    StartMaintenanceThread();
    spdlog::info("Master listening on {}:{}", config_.host, config_.port);
    bool ok = server_->listen(config_.host.c_str(), config_.port);
    StopMaintenanceThread();

    if (!ok) {
        spdlog::error("Failed to bind to {}:{}", config_.host, config_.port);
        return 2;
    }
    return 0;
}

}  // namespace master
}  // namespace dc
