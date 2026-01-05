#include "control_service.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

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

bool IsValidAgentStatus(const std::string& status) {
    std::string lower = status;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "idle" || lower == "busy" || lower == "offline";
}

bool IsValidTaskState(const std::string& state) {
    std::string lower = state;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "queued" || lower == "running" || lower == "succeeded" ||
           lower == "failed" || lower == "canceled";
}

bool IsValidTaskId(const std::string& task_id) {
    if (task_id.empty() || task_id.size() > 128) {
        return false;
    }
    for (char c : task_id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool IsValidTaskStateTransition(const std::string& from, const std::string& to) {
    std::string from_lower = from;
    std::string to_lower = to;
    std::transform(from_lower.begin(), from_lower.end(), from_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(to_lower.begin(), to_lower.end(), to_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (from_lower == to_lower) {
        return true;
    }
    if (from_lower == "queued") {
        return to_lower == "running" || to_lower == "canceled";
    }
    if (from_lower == "running") {
        return to_lower == "succeeded" || to_lower == "failed" || to_lower == "canceled";
    }
    return false;
}

}  // namespace

ControlService::ControlService(MasterConfig config, Storage storage, LogStore log_store)
    : config_(std::move(config)),
      storage_(std::move(storage)),
      log_store_(std::move(log_store)) {}

ControlService::~ControlService() = default;

void ControlService::RegisterRoutes() {
    // All endpoints live under /api/v1 to keep versioned compatibility with future changes.
    server_->Put(R"(/api/v1/agents/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agent_id = req.matches[1];
        json body;
        std::string error;
        if (!ParseJsonBody(req, &body, &error)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
            return;
        }

        if (!body.contains("os") || !body["os"].is_string() ||
            !body.contains("version") || !body["version"].is_string() ||
            !body.contains("resources") || !body["resources"].is_object()) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing required fields"), 400);
            return;
        }

        const auto& resources = body["resources"];
        if (!resources.contains("cpu_cores") || !resources["cpu_cores"].is_number_integer() ||
            !resources.contains("ram_mb") || !resources["ram_mb"].is_number_integer() ||
            !resources.contains("slots") || !resources["slots"].is_number_integer()) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid resources payload"), 400);
            return;
        }

        AgentInput agent;
        agent.agent_id = agent_id;
        agent.os = body["os"].get<std::string>();
        agent.version = body["version"].get<std::string>();
        agent.cpu_cores = resources["cpu_cores"].get<int>();
        agent.ram_mb = resources["ram_mb"].get<int>();
        agent.slots = resources["slots"].get<int>();

        try {
            storage_.UpsertAgent(agent);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        json response;
        response["status"] = "ok";
        response["heartbeat_interval_sec"] = config_.heartbeat_interval_sec;
        SetJsonResponse(res, response, 200);
    });

    server_->Post(R"(/api/v1/agents/([^/]+)/heartbeat)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agent_id = req.matches[1];
        json body;
        std::string error;
        if (!ParseJsonBody(req, &body, &error)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
            return;
        }

        if (!body.contains("status") || !body["status"].is_string()) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing status"), 400);
            return;
        }

        std::string status = body["status"].get<std::string>();
        if (!IsValidAgentStatus(status)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid agent status"), 400);
            return;
        }

        AgentHeartbeat heartbeat;
        heartbeat.agent_id = agent_id;
        heartbeat.status = status;

        try {
            if (!storage_.UpdateHeartbeat(heartbeat)) {
                SetJsonResponse(res, MakeError("AGENT_NOT_FOUND", "Agent not found",
                                               {{"agent_id", agent_id}}), 404);
                return;
            }
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        SetJsonResponse(res, json{{"status", "ok"}}, 200);
    });

    server_->Post(R"(/api/v1/agents/([^/]+)/tasks:poll)",
                  [this](const httplib::Request& req, httplib::Response& res) {
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
            dispatches = storage_.PollTasksForAgent(agent_id, free_slots);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        if (!dispatches) {
            SetJsonResponse(res, MakeError("AGENT_NOT_FOUND", "Agent not found",
                                           {{"agent_id", agent_id}}), 404);
            return;
        }

        json response;
        response["tasks"] = json::array();
        for (const auto& task : *dispatches) {
            json item;
            item["task_id"] = task.task_id;
            item["command"] = task.command;
            item["args"] = task.args;
            item["env"] = task.env;
            if (task.timeout_sec) {
                item["timeout_sec"] = *task.timeout_sec;
            }
            if (!task.constraints.is_null()) {
                item["constraints"] = task.constraints;
            }
            response["tasks"].push_back(item);
        }
        SetJsonResponse(res, response, 200);
    });

    server_->Post(R"(/api/v1/tasks)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!ParseJsonBody(req, &body, &error)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
            return;
        }

        if (!body.contains("task_id") || !body["task_id"].is_string()) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing task_id"), 400);
            return;
        }
        if (!body.contains("command") || !body["command"].is_string()) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing command"), 400);
            return;
        }

        TaskInput task;
        task.task_id = body["task_id"].get<std::string>();
        if (!IsValidTaskId(task.task_id)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
            return;
        }
        task.command = body["command"].get<std::string>();
        task.args = (body.contains("args") && body["args"].is_array()) ? body["args"] : json::array();
        task.env = (body.contains("env") && body["env"].is_object()) ? body["env"] : json::object();
        if (body.contains("timeout_sec") && body["timeout_sec"].is_number_integer()) {
            task.timeout_sec = body["timeout_sec"].get<int>();
        }
        if (body.contains("constraints") && body["constraints"].is_object()) {
            task.constraints = body["constraints"];
        } else {
            task.constraints = json::object();
        }

        CreateTaskResult result;
        try {
            result = storage_.CreateTask(task);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        if (result == CreateTaskResult::AlreadyExists) {
            SetJsonResponse(res, MakeError("TASK_EXISTS", "Task already exists",
                                           {{"task_id", task.task_id}}), 409);
            return;
        }
        if (result != CreateTaskResult::Ok) {
            SetJsonResponse(res, MakeError("DB_ERROR", "Failed to create task"), 500);
            return;
        }

        json response;
        response["task_id"] = task.task_id;
        SetJsonResponse(res, response, 201);
    });

    server_->Get(R"(/api/v1/tasks/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
        const std::string task_id = req.matches[1];
        if (!IsValidTaskId(task_id)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
            return;
        }
        std::optional<TaskRecord> task;
        try {
            task = storage_.GetTask(task_id);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        if (!task) {
            SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                           {{"task_id", task_id}}), 404);
            return;
        }

        json payload;
        payload["task_id"] = task->task_id;
        payload["state"] = task->state;
        payload["command"] = task->command;
        payload["args"] = task->args;
        payload["env"] = task->env;
        if (task->timeout_sec) {
            payload["timeout_sec"] = *task->timeout_sec;
        }
        if (task->assigned_agent) {
            payload["assigned_agent"] = *task->assigned_agent;
        }
        payload["created_at"] = task->created_at;
        if (task->started_at) {
            payload["started_at"] = *task->started_at;
        }
        if (task->finished_at) {
            payload["finished_at"] = *task->finished_at;
        }
        if (task->exit_code) {
            payload["exit_code"] = *task->exit_code;
        }
        if (task->error_message) {
            payload["error_message"] = *task->error_message;
        }
        payload["constraints"] = task->constraints;

        SetJsonResponse(res, json{{"task", payload}}, 200);
    });

    server_->Get(R"(/api/v1/tasks)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        auto state = GetQueryParam(req, "state");
        if (state && !IsValidTaskState(*state)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task state"), 400);
            return;
        }

        auto agent_id = GetQueryParam(req, "agent_id");
        int limit = GetQueryParamInt(req, "limit", 50);
        int offset = GetQueryParamInt(req, "offset", 0);

        std::vector<TaskSummary> tasks;
        try {
            tasks = storage_.ListTasks(state, agent_id, limit, offset);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        json response;
        response["tasks"] = json::array();
        for (const auto& task : tasks) {
            response["tasks"].push_back({{"task_id", task.task_id}, {"state", task.state}});
        }
        SetJsonResponse(res, response, 200);
    });

    server_->Post(R"(/api/v1/tasks/([^/]+)/status)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        const std::string task_id = req.matches[1];
        if (!IsValidTaskId(task_id)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
            return;
        }
        json body;
        std::string error;
        if (!ParseJsonBody(req, &body, &error)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid JSON", {{"error", error}}), 400);
            return;
        }

        if (!body.contains("state") || !body["state"].is_string()) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Missing state"), 400);
            return;
        }

        std::string state = body["state"].get<std::string>();
        if (!IsValidTaskState(state)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task state"), 400);
            return;
        }

        std::optional<TaskRecord> current;
        try {
            current = storage_.GetTask(task_id);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }
        if (!current) {
            SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                           {{"task_id", task_id}}), 404);
            return;
        }
        if (!IsValidTaskStateTransition(current->state, state)) {
            SetJsonResponse(res, MakeError("INVALID_STATE_TRANSITION",
                                           "Invalid task state transition",
                                           {{"from", current->state}, {"to", state}}),
                            409);
            return;
        }

        std::optional<int> exit_code;
        std::optional<std::string> started_at;
        std::optional<std::string> finished_at;
        std::optional<std::string> error_message;

        if (body.contains("exit_code") && body["exit_code"].is_number_integer()) {
            exit_code = body["exit_code"].get<int>();
        }
        if (body.contains("started_at") && body["started_at"].is_string()) {
            started_at = body["started_at"].get<std::string>();
        }
        if (body.contains("finished_at") && body["finished_at"].is_string()) {
            finished_at = body["finished_at"].get<std::string>();
        }
        if (body.contains("error_message") && body["error_message"].is_string()) {
            error_message = body["error_message"].get<std::string>();
        }

        try {
            if (!storage_.UpdateTaskStatus(task_id, state, exit_code, started_at,
                                           finished_at, error_message)) {
                SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                               {{"task_id", task_id}}), 404);
                return;
            }
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        SetJsonResponse(res, json{{"status", "ok"}}, 200);
    });

    server_->Post(R"(/api/v1/tasks/([^/]+)/cancel)",
                  [this](const httplib::Request& req, httplib::Response& res) {
        const std::string task_id = req.matches[1];
        if (!IsValidTaskId(task_id)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
            return;
        }
        CancelTaskResult result;
        try {
            result = storage_.CancelTask(task_id);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        if (result == CancelTaskResult::NotFound) {
            SetJsonResponse(res, MakeError("TASK_NOT_FOUND", "Task not found",
                                           {{"task_id", task_id}}), 404);
            return;
        }
        if (result == CancelTaskResult::InvalidState) {
            SetJsonResponse(res, MakeError("TASK_INVALID_STATE",
                                           "Task already finished; cannot cancel",
                                           {{"task_id", task_id}}),
                            409);
            return;
        }
        if (result != CancelTaskResult::Ok) {
            SetJsonResponse(res, MakeError("DB_ERROR", "Failed to cancel task"), 500);
            return;
        }

        SetJsonResponse(res, json{{"status", "ok"}}, 200);
    });

    server_->Get(R"(/api/v1/agents/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agent_id = req.matches[1];
        std::optional<AgentRecord> agent;
        try {
            agent = storage_.GetAgent(agent_id);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        if (!agent) {
            SetJsonResponse(res, MakeError("AGENT_NOT_FOUND", "Agent not found",
                                           {{"agent_id", agent_id}}), 404);
            return;
        }

        json payload;
        payload["agent_id"] = agent->agent_id;
        payload["os"] = agent->os;
        payload["version"] = agent->version;
        payload["resources"] = {
            {"cpu_cores", agent->cpu_cores},
            {"ram_mb", agent->ram_mb},
            {"slots", agent->slots},
        };
        payload["status"] = agent->status;
        payload["last_heartbeat"] = agent->last_heartbeat;
        SetJsonResponse(res, json{{"agent", payload}}, 200);
    });

    server_->Get(R"(/api/v1/agents)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        auto status = GetQueryParam(req, "status");
        if (status && !IsValidAgentStatus(*status)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid agent status"), 400);
            return;
        }
        int limit = GetQueryParamInt(req, "limit", 50);
        int offset = GetQueryParamInt(req, "offset", 0);

        std::vector<AgentRecord> agents;
        try {
            agents = storage_.ListAgents(status, limit, offset);
        } catch (const std::exception& ex) {
            SetJsonResponse(res, MakeError("DB_ERROR", ex.what()), 500);
            return;
        }

        json response;
        response["agents"] = json::array();
        for (const auto& agent : agents) {
            response["agents"].push_back({{"agent_id", agent.agent_id}, {"status", agent.status}});
        }
        SetJsonResponse(res, response, 200);
    });

    // Logs are served from file storage; metadata is updated on each read.
    server_->Get(R"(/api/v1/tasks/([^/]+)/logs)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        const std::string task_id = req.matches[1];
        if (!IsValidTaskId(task_id)) {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid task_id"), 400);
            return;
        }
        std::string stream = GetQueryParam(req, "stream").value_or("stdout");
        if (stream != "stdout" && stream != "stderr") {
            SetJsonResponse(res, MakeError("BAD_REQUEST", "Invalid stream"), 400);
            return;
        }

        auto log_result = log_store_.ReadAll(task_id, stream);
        if (!log_result.exists) {
            SetJsonResponse(res, MakeError("LOG_NOT_FOUND", "Log not found",
                                           {{"task_id", task_id}, {"stream", stream}}), 404);
            return;
        }

        res.status = 200;
        res.set_content(log_result.data, "text/plain; charset=utf-8");
    });

    server_->Get(R"(/api/v1/tasks/([^/]+)/logs:tail)",
                 [this](const httplib::Request& req, httplib::Response& res) {
        const std::string task_id = req.matches[1];
        if (!IsValidTaskId(task_id)) {
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

        auto log_result = log_store_.ReadFromOffset(task_id, stream, offset);
        if (!log_result.exists) {
            SetJsonResponse(res, MakeError("LOG_NOT_FOUND", "Log not found",
                                           {{"task_id", task_id}, {"stream", stream}}), 404);
            return;
        }

        res.status = 200;
        res.set_header("X-Log-Size", std::to_string(log_result.size_bytes));
        res.set_content(log_result.data, "text/plain; charset=utf-8");
    });
}

void ControlService::StartMaintenanceThread() {
    running_.store(true);
    maintenance_thread_ = std::thread([this]() {
        while (running_.load()) {
            try {
                storage_.MarkOfflineAgentsAndRequeue(config_.offline_after_sec);
            } catch (const std::exception& ex) {
                std::cerr << "maintenance error: " << ex.what() << std::endl;
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

    RegisterRoutes();

    StartMaintenanceThread();
    std::cout << "Master listening on " << config_.host << ":" << config_.port << std::endl;
    bool ok = server_->listen(config_.host.c_str(), config_.port);
    StopMaintenanceThread();

    if (!ok) {
        std::cerr << "Failed to bind to " << config_.host << ":" << config_.port << std::endl;
        return 2;
    }
    return 0;
}

}  // namespace master
}  // namespace dc
