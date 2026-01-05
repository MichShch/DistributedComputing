#include "storage.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <pqxx/pqxx>

namespace dc {
namespace master {

using json = nlohmann::json;

namespace {

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace

Storage::Storage(DbConfig config) : config_(std::move(config)) {}

std::string Storage::ConnectionString() const {
    std::ostringstream out;
    if (!config_.host.empty()) {
        out << "host=" << config_.host << " ";
    }
    if (!config_.port.empty()) {
        out << "port=" << config_.port << " ";
    }
    if (!config_.user.empty()) {
        out << "user=" << config_.user << " ";
    }
    if (!config_.password.empty()) {
        out << "password=" << config_.password << " ";
    }
    if (!config_.dbname.empty()) {
        out << "dbname=" << config_.dbname << " ";
    }
    if (!config_.sslmode.empty()) {
        out << "sslmode=" << config_.sslmode << " ";
    }
    return out.str();
}

json Storage::SafeParseJson(const std::string& raw, const json& fallback) const {
    if (raw.empty()) {
        return fallback;
    }
    try {
        return json::parse(raw);
    } catch (...) {
        return fallback;
    }
}

std::string Storage::DbAgentStatusFromApi(const std::string& status) const {
    std::string normalized = ToLowerCopy(status);
    if (normalized == "idle") {
        return "Idle";
    }
    if (normalized == "busy") {
        return "Busy";
    }
    if (normalized == "offline") {
        return "Offline";
    }
    return "Idle";
}

std::string Storage::ApiAgentStatusFromDb(const std::string& status) const {
    return ToLowerCopy(status);
}

std::string Storage::DbTaskStateFromApi(const std::string& state) const {
    std::string normalized = ToLowerCopy(state);
    if (normalized == "queued") {
        return "Queued";
    }
    if (normalized == "running") {
        return "Running";
    }
    if (normalized == "succeeded") {
        return "Succeeded";
    }
    if (normalized == "failed") {
        return "Failed";
    }
    if (normalized == "canceled") {
        return "Canceled";
    }
    return "Queued";
}

std::string Storage::ApiTaskStateFromDb(const std::string& state) const {
    return ToLowerCopy(state);
}

bool Storage::UpsertAgent(const AgentInput& agent) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);
    tx.exec_params(
        "INSERT INTO agents (agent_id, os, version, resources_cpu_cores, resources_ram_mb, "
        "resources_slots, status, last_heartbeat) "
        "VALUES ($1, $2, $3, $4, $5, $6, 'Idle', NOW()) "
        "ON CONFLICT (agent_id) DO UPDATE SET "
        "os = EXCLUDED.os, "
        "version = EXCLUDED.version, "
        "resources_cpu_cores = EXCLUDED.resources_cpu_cores, "
        "resources_ram_mb = EXCLUDED.resources_ram_mb, "
        "resources_slots = EXCLUDED.resources_slots, "
        "status = 'Idle', "
        "last_heartbeat = NOW()",
        agent.agent_id,
        agent.os,
        agent.version,
        agent.cpu_cores,
        agent.ram_mb,
        agent.slots);
    tx.commit();
    return true;
}

bool Storage::UpdateHeartbeat(const AgentHeartbeat& heartbeat) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);
    auto result = tx.exec_params(
        "UPDATE agents SET status = $1, last_heartbeat = NOW() WHERE agent_id = $2",
        DbAgentStatusFromApi(heartbeat.status),
        heartbeat.agent_id);
    if (result.affected_rows() == 0) {
        return false;
    }
    tx.commit();
    return true;
}

std::optional<AgentRecord> Storage::GetAgent(const std::string& agent_id) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);
    auto result = tx.exec_params(
        "SELECT agent_id, os, version, resources_cpu_cores, resources_ram_mb, "
        "resources_slots, status::text, "
        "to_char(last_heartbeat at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "AS last_heartbeat "
        "FROM agents WHERE agent_id = $1",
        agent_id);
    if (result.empty()) {
        return std::nullopt;
    }
    const auto& row = result[0];
    AgentRecord record;
    record.agent_id = row["agent_id"].c_str();
    record.os = row["os"].c_str();
    record.version = row["version"].c_str();
    record.cpu_cores = row["resources_cpu_cores"].as<int>();
    record.ram_mb = row["resources_ram_mb"].as<int>();
    record.slots = row["resources_slots"].as<int>();
    record.status = ApiAgentStatusFromDb(row["status"].c_str());
    record.last_heartbeat = row["last_heartbeat"].c_str();
    return record;
}

std::vector<AgentRecord> Storage::ListAgents(const std::optional<std::string>& status,
                                             int limit,
                                             int offset) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    std::string query =
        "SELECT agent_id, os, version, resources_cpu_cores, resources_ram_mb, "
        "resources_slots, status::text, "
        "to_char(last_heartbeat at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "AS last_heartbeat "
        "FROM agents";

    if (status) {
        query += " WHERE status = " + tx.quote(DbAgentStatusFromApi(*status));
    }
    query += " ORDER BY agent_id";
    query += " LIMIT " + std::to_string(limit);
    query += " OFFSET " + std::to_string(offset);

    auto result = tx.exec(query);
    std::vector<AgentRecord> agents;
    agents.reserve(result.size());
    for (const auto& row : result) {
        AgentRecord record;
        record.agent_id = row["agent_id"].c_str();
        record.os = row["os"].c_str();
        record.version = row["version"].c_str();
        record.cpu_cores = row["resources_cpu_cores"].as<int>();
        record.ram_mb = row["resources_ram_mb"].as<int>();
        record.slots = row["resources_slots"].as<int>();
        record.status = ApiAgentStatusFromDb(row["status"].c_str());
        record.last_heartbeat = row["last_heartbeat"].c_str();
        agents.push_back(std::move(record));
    }
    return agents;
}

CreateTaskResult Storage::CreateTask(const TaskInput& task) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    auto exists = tx.exec_params("SELECT 1 FROM tasks WHERE task_id = $1", task.task_id);
    if (!exists.empty()) {
        return CreateTaskResult::AlreadyExists;
    }

    std::string args_json = task.args.is_null() ? "[]" : task.args.dump();
    std::string env_json = task.env.is_null() ? "{}" : task.env.dump();

    std::string timeout_sql = task.timeout_sec ? tx.quote(*task.timeout_sec) : "NULL";

    // Insert task metadata first, then constraints to keep schema consistent.
    std::string insert_task_query =
        "INSERT INTO tasks (task_id, state, command, args, env, timeout_sec, "
        "assigned_agent, created_at) VALUES (" +
        tx.quote(task.task_id) + ", 'Queued', " + tx.quote(task.command) + ", " +
        tx.quote(args_json) + "::jsonb, " + tx.quote(env_json) + "::jsonb, " +
        timeout_sql + ", NULL, NOW())";
    tx.exec(insert_task_query);

    std::optional<std::string> os;
    std::optional<int> cpu_cores;
    std::optional<int> ram_mb;
    std::optional<std::string> labels_json;
    if (task.constraints.is_object()) {
        if (task.constraints.contains("os") && task.constraints["os"].is_string()) {
            os = task.constraints["os"].get<std::string>();
        }
        if (task.constraints.contains("cpu_cores") && task.constraints["cpu_cores"].is_number_integer()) {
            cpu_cores = task.constraints["cpu_cores"].get<int>();
        }
        if (task.constraints.contains("ram_mb") && task.constraints["ram_mb"].is_number_integer()) {
            ram_mb = task.constraints["ram_mb"].get<int>();
        }
        if (task.constraints.contains("labels") && task.constraints["labels"].is_array()) {
            labels_json = task.constraints["labels"].dump();
        }
    }

    std::string insert_constraints_query =
        "INSERT INTO task_constraints (task_id, os, cpu_cores, ram_mb, labels) VALUES (" +
        tx.quote(task.task_id) + ", " +
        (os ? tx.quote(*os) : "NULL") + ", " +
        (cpu_cores ? tx.quote(*cpu_cores) : "NULL") + ", " +
        (ram_mb ? tx.quote(*ram_mb) : "NULL") + ", " +
        (labels_json ? tx.quote(*labels_json) + "::jsonb" : "NULL") + ")";
    tx.exec(insert_constraints_query);

    tx.commit();
    return CreateTaskResult::Ok;
}

std::optional<TaskRecord> Storage::GetTask(const std::string& task_id) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);
    auto result = tx.exec_params(
        "SELECT t.task_id, t.state::text, t.command, t.args::text, t.env::text, "
        "t.timeout_sec, t.assigned_agent, "
        "to_char(t.created_at at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "AS created_at, "
        "to_char(t.started_at at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "AS started_at, "
        "to_char(t.finished_at at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "AS finished_at, "
        "t.exit_code, t.error_message, c.os, c.cpu_cores, c.ram_mb, c.labels::text "
        "FROM tasks t "
        "LEFT JOIN task_constraints c ON t.task_id = c.task_id "
        "WHERE t.task_id = $1",
        task_id);
    if (result.empty()) {
        return std::nullopt;
    }

    const auto& row = result[0];
    TaskRecord record;
    record.task_id = row["task_id"].c_str();
    record.state = ApiTaskStateFromDb(row["state"].c_str());
    record.command = row["command"].c_str();
    record.args = SafeParseJson(row["args"].c_str(), json::array());
    record.env = SafeParseJson(row["env"].c_str(), json::object());
    if (!row["timeout_sec"].is_null()) {
        record.timeout_sec = row["timeout_sec"].as<int>();
    }
    if (!row["assigned_agent"].is_null()) {
        record.assigned_agent = row["assigned_agent"].c_str();
    }
    record.created_at = row["created_at"].c_str();
    if (!row["started_at"].is_null()) {
        record.started_at = row["started_at"].c_str();
    }
    if (!row["finished_at"].is_null()) {
        record.finished_at = row["finished_at"].c_str();
    }
    if (!row["exit_code"].is_null()) {
        record.exit_code = row["exit_code"].as<int>();
    }
    if (!row["error_message"].is_null()) {
        record.error_message = row["error_message"].c_str();
    }

    json constraints = json::object();
    if (!row["os"].is_null()) {
        constraints["os"] = row["os"].c_str();
    }
    if (!row["cpu_cores"].is_null()) {
        constraints["cpu_cores"] = row["cpu_cores"].as<int>();
    }
    if (!row["ram_mb"].is_null()) {
        constraints["ram_mb"] = row["ram_mb"].as<int>();
    }
    if (!row["labels"].is_null()) {
        constraints["labels"] = SafeParseJson(row["labels"].c_str(), json::array());
    }
    record.constraints = constraints;
    return record;
}

std::vector<TaskSummary> Storage::ListTasks(const std::optional<std::string>& state,
                                            const std::optional<std::string>& agent_id,
                                            int limit,
                                            int offset) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    std::string query = "SELECT task_id, state::text FROM tasks";
    std::vector<std::string> filters;
    if (state) {
        filters.push_back("state = " + tx.quote(DbTaskStateFromApi(*state)));
    }
    if (agent_id) {
        filters.push_back("assigned_agent = " + tx.quote(*agent_id));
    }
    if (!filters.empty()) {
        query += " WHERE " + filters[0];
        for (size_t i = 1; i < filters.size(); ++i) {
            query += " AND " + filters[i];
        }
    }
    query += " ORDER BY created_at DESC";
    query += " LIMIT " + std::to_string(limit);
    query += " OFFSET " + std::to_string(offset);

    auto result = tx.exec(query);
    std::vector<TaskSummary> tasks;
    tasks.reserve(result.size());
    for (const auto& row : result) {
        TaskSummary summary;
        summary.task_id = row["task_id"].c_str();
        summary.state = ApiTaskStateFromDb(row["state"].c_str());
        tasks.push_back(std::move(summary));
    }
    return tasks;
}

std::optional<std::vector<TaskDispatch>> Storage::PollTasksForAgent(
    const std::string& agent_id,
    int free_slots) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    auto agent_result = tx.exec_params(
        "SELECT os, resources_cpu_cores, resources_ram_mb FROM agents WHERE agent_id = $1",
        agent_id);
    if (agent_result.empty()) {
        return std::nullopt;
    }

    const auto& agent_row = agent_result[0];
    std::string agent_os = agent_row["os"].c_str();
    int agent_cpu = agent_row["resources_cpu_cores"].as<int>();
    int agent_ram = agent_row["resources_ram_mb"].as<int>();

    tx.exec_params("UPDATE agents SET last_heartbeat = NOW() WHERE agent_id = $1", agent_id);

    if (free_slots <= 0) {
        tx.commit();
        return std::vector<TaskDispatch>{};
    }

    // FIFO scheduling with OS/CPU/RAM filters; SKIP LOCKED avoids double assignment.
    auto tasks_result = tx.exec_params(
        "SELECT t.task_id, t.command, t.args::text, t.env::text, t.timeout_sec, "
        "c.os, c.cpu_cores, c.ram_mb, c.labels::text "
        "FROM tasks t "
        "LEFT JOIN task_constraints c ON t.task_id = c.task_id "
        "WHERE t.state = 'Queued' AND t.assigned_agent IS NULL "
        "AND (c.os IS NULL OR c.os = $1) "
        "AND (c.cpu_cores IS NULL OR c.cpu_cores <= $2) "
        "AND (c.ram_mb IS NULL OR c.ram_mb <= $3) "
        "ORDER BY t.created_at "
        "FOR UPDATE SKIP LOCKED "
        "LIMIT $4",
        agent_os,
        agent_cpu,
        agent_ram,
        free_slots);

    std::vector<TaskDispatch> dispatches;
    dispatches.reserve(tasks_result.size());

    for (const auto& row : tasks_result) {
        TaskDispatch dispatch;
        dispatch.task_id = row["task_id"].c_str();
        dispatch.command = row["command"].c_str();
        dispatch.args = SafeParseJson(row["args"].c_str(), json::array());
        dispatch.env = SafeParseJson(row["env"].c_str(), json::object());
        if (!row["timeout_sec"].is_null()) {
            dispatch.timeout_sec = row["timeout_sec"].as<int>();
        }

        json constraints = json::object();
        if (!row["os"].is_null()) {
            constraints["os"] = row["os"].c_str();
        }
        if (!row["cpu_cores"].is_null()) {
            constraints["cpu_cores"] = row["cpu_cores"].as<int>();
        }
        if (!row["ram_mb"].is_null()) {
            constraints["ram_mb"] = row["ram_mb"].as<int>();
        }
        if (!row["labels"].is_null()) {
            constraints["labels"] = SafeParseJson(row["labels"].c_str(), json::array());
        }
        dispatch.constraints = constraints;

        dispatches.push_back(std::move(dispatch));
    }

    // Task assignment is part of the same transaction, so workers never see duplicates.
    for (const auto& dispatch : dispatches) {
        tx.exec_params(
            "UPDATE tasks SET state = 'Running', assigned_agent = $1, started_at = NOW() "
            "WHERE task_id = $2",
            agent_id,
            dispatch.task_id);
        tx.exec_params(
            "INSERT INTO task_assignments (task_id, agent_id, assigned_at) "
            "VALUES ($1, $2, NOW())",
            dispatch.task_id,
            agent_id);
    }

    if (!dispatches.empty()) {
        tx.exec_params("UPDATE agents SET status = 'Busy' WHERE agent_id = $1", agent_id);
    } else {
        tx.exec_params("UPDATE agents SET status = 'Idle' WHERE agent_id = $1", agent_id);
    }

    tx.commit();
    return dispatches;
}

bool Storage::UpdateTaskStatus(const std::string& task_id,
                               const std::string& state,
                               const std::optional<int>& exit_code,
                               const std::optional<std::string>& started_at,
                               const std::optional<std::string>& finished_at,
                               const std::optional<std::string>& error_message) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    std::string db_state = DbTaskStateFromApi(state);

    std::string started_expr = "started_at";
    if (started_at) {
        started_expr = tx.quote(*started_at);
    } else if (db_state == "Running") {
        started_expr = "NOW()";
    }

    std::string finished_expr = "finished_at";
    if (finished_at) {
        finished_expr = tx.quote(*finished_at);
    } else if (db_state == "Succeeded" || db_state == "Failed" || db_state == "Canceled") {
        finished_expr = "NOW()";
    }

    std::string exit_expr = exit_code ? tx.quote(*exit_code) : "exit_code";
    std::string error_expr = error_message ? tx.quote(*error_message) : "error_message";

    std::string update_task_query =
        "UPDATE tasks SET state = " + tx.quote(db_state) + ", "
        "exit_code = " + exit_expr + ", "
        "started_at = " + started_expr + ", "
        "finished_at = " + finished_expr + ", "
        "error_message = " + error_expr +
        " WHERE task_id = " + tx.quote(task_id);
    auto result = tx.exec(update_task_query);
    if (result.affected_rows() == 0) {
        return false;
    }

    if (db_state == "Succeeded" || db_state == "Failed" || db_state == "Canceled") {
        std::string reason = (db_state == "Canceled") ? "canceled" : "completed";
        std::string update_assignments_query =
            "UPDATE task_assignments SET unassigned_at = " + finished_expr +
            ", reason = " + tx.quote(reason) +
            " WHERE task_id = " + tx.quote(task_id) +
            " AND unassigned_at IS NULL";
        tx.exec(update_assignments_query);
    }

    tx.commit();
    return true;
}

CancelTaskResult Storage::CancelTask(const std::string& task_id) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    auto result = tx.exec_params(
        "SELECT state::text FROM tasks WHERE task_id = $1",
        task_id);
    if (result.empty()) {
        return CancelTaskResult::NotFound;
    }

    std::string state = result[0]["state"].c_str();
    if (state == "Succeeded" || state == "Failed" || state == "Canceled") {
        return CancelTaskResult::InvalidState;
    }

    std::string cancel_task_query =
        "UPDATE tasks SET state = 'Canceled', finished_at = NOW(), "
        "error_message = COALESCE(error_message, 'canceled_by_user') "
        "WHERE task_id = " + tx.quote(task_id);
    tx.exec(cancel_task_query);

    std::string cancel_assignments_query =
        "UPDATE task_assignments SET unassigned_at = NOW(), reason = 'canceled_by_user' "
        "WHERE task_id = " + tx.quote(task_id) + " AND unassigned_at IS NULL";
    tx.exec(cancel_assignments_query);

    tx.commit();
    return CancelTaskResult::Ok;
}

int Storage::MarkOfflineAgentsAndRequeue(int offline_after_sec) {
    pqxx::connection conn(ConnectionString());
    pqxx::work tx(conn);

    std::string threshold = "NOW() - interval '" + std::to_string(offline_after_sec) + " seconds'";
    // Agents past the heartbeat threshold are marked offline, and their tasks are requeued.
    std::string select_agents_query =
        "SELECT agent_id FROM agents "
        "WHERE status <> 'Offline' AND last_heartbeat < " + threshold +
        " FOR UPDATE";
    auto agents = tx.exec(select_agents_query);

    if (agents.empty()) {
        return 0;
    }

    for (const auto& row : agents) {
        std::string agent_id = row["agent_id"].c_str();
        std::string update_agent_query =
            "UPDATE agents SET status = 'Offline' WHERE agent_id = " + tx.quote(agent_id);
        tx.exec(update_agent_query);

        std::string update_assignments_query =
            "UPDATE task_assignments SET unassigned_at = NOW(), reason = 'agent_offline' "
            "WHERE agent_id = " + tx.quote(agent_id) + " AND unassigned_at IS NULL";
        tx.exec(update_assignments_query);

        std::string requeue_tasks_query =
            "UPDATE tasks SET state = 'Queued', assigned_agent = NULL, started_at = NULL, "
            "error_message = COALESCE(error_message, 'agent_offline_requeued') "
            "WHERE assigned_agent = " + tx.quote(agent_id) +
            " AND state IN ('Running', 'Queued')";
        tx.exec(requeue_tasks_query);
    }

    tx.commit();
    return static_cast<int>(agents.size());
}

}  // namespace master
}  // namespace dc
