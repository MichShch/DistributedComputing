#include "pg_broker.hpp"

#include <format>

#include <pqxx/pqxx>

#include "common/logging.hpp"
#include "status.hpp"

namespace dc {
namespace broker {

using json = nlohmann::json;

namespace {

json ParseJsonOrDefault(const std::string& raw, const json& fallback) {
    if (raw.empty()) {
        return fallback;
    }
    try {
        return json::parse(raw);
    } catch (...) {
        return fallback;
    }
}

json NormalizeConstraints(const json& input) {
    json constraints = json::object();
    if (!input.is_object()) {
        return constraints;
    }
    if (input.contains("os") && input["os"].is_string()) {
        constraints["os"] = input["os"];
    }
    if (input.contains("cpu_cores") && input["cpu_cores"].is_number_integer()) {
        constraints["cpu_cores"] = input["cpu_cores"];
    }
    if (input.contains("ram_mb") && input["ram_mb"].is_number_integer()) {
        constraints["ram_mb"] = input["ram_mb"];
    }
    if (input.contains("labels") && input["labels"].is_array()) {
        constraints["labels"] = input["labels"];
    }
    return constraints;
}

json BuildConstraintsFromRow(const pqxx::row& row) {
    if (row["constraints"].is_null()) {
        return json::object();
    }
    return NormalizeConstraints(ParseJsonOrDefault(row["constraints"].c_str(), json::object()));
}

}  // namespace

PgBroker::PgBroker(DbConfig& config)
    : Broker(config, BrokerType::PGSQL, GenerateConnectionString(config)) {
    ExecuteWithRetry("postgres startup connect", [this]() {
        pqxx::connection conn(connectionString_);
        if (!conn.is_open()) {
            throw std::runtime_error("Postgres connection is not open");
        }
    });
}

std::string PgBroker::GenerateConnectionString(
        const DbConfig& config
    ) const {
    std::ostringstream out;
    out << "host=" << config.host << " ";
    out << "port=" << config.port << " ";
    out << "dbname=" << config.dbname << " ";

    switch(config.authMethod) {
        case AuthentificationMethod::PASSWORD:
            out << "user=" << config.user << " ";
            out << "password=" << config.password << " ";
            break;
        case AuthentificationMethod::SSL:
            out << "sslmode=" << "verify-full" << " ";
            out << "sslrootcert=" << config.ssl.rootcert << " ";
            out << "sslcert=" << config.ssl.cert << " ";
            out << "sslkey=" << config.ssl.key << " ";
            break;
        default:
            throw std::runtime_error("Unexpected case");
    }
    return out.str();
}

bool PgBroker::UpsertAgent(const AgentInput& agent) {
    return ExecuteWithRetry("postgres upsert_agent", [&]() {
        pqxx::connection conn(connectionString_);
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
    });
}

bool PgBroker::UpdateHeartbeat(const AgentHeartbeat& heartbeat) {
    return ExecuteWithRetry("postgres update_heartbeat", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);
        auto result = tx.exec_params(
            "UPDATE agents SET status = $1, last_heartbeat = NOW() WHERE agent_id = $2",
            AgentStatusToDb(heartbeat.status),
            heartbeat.agent_id);
        if (result.affected_rows() == 0) {
            return false;
        }
        tx.commit();
        return true;
    });
}

std::optional<AgentRecord> PgBroker::GetAgent(const std::string& agent_id) {
    return ExecuteWithRetry("postgres get_agent", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);
        auto result = tx.exec_params(
            "SELECT agent_id, os, version, resources_cpu_cores, resources_ram_mb, "
            "resources_slots, status::text, "
            "to_char(last_heartbeat at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
            "AS last_heartbeat "
            "FROM agents WHERE agent_id = $1",
            agent_id);
        if (result.empty()) {
            return std::optional<AgentRecord>{std::nullopt};
        }
        const auto& row = result[0];
        AgentRecord record;
        record.agent_id = row["agent_id"].c_str();
        record.os = row["os"].c_str();
        record.version = row["version"].c_str();
        record.cpu_cores = row["resources_cpu_cores"].as<int>();
        record.ram_mb = row["resources_ram_mb"].as<int>();
        record.slots = row["resources_slots"].as<int>();
        record.status = AgentStatusFromApi(row["status"].c_str()).value_or(AgentStatus::Idle);
        record.last_heartbeat = row["last_heartbeat"].c_str();
        return std::optional<AgentRecord>{record};
    });
}

std::vector<AgentRecord> PgBroker::ListAgents(const std::optional<AgentStatus>& status,
                                             int limit,
                                             int offset) {
    return ExecuteWithRetry("postgres list_agents", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);
        std::string db_status;
        const char* status_param = nullptr;
        if (status) {
            db_status = AgentStatusToDb(*status);
            status_param = db_status.c_str();
        }

        auto result = tx.exec_params(
            "SELECT agent_id, os, version, resources_cpu_cores, resources_ram_mb, "
            "resources_slots, status::text, "
            "to_char(last_heartbeat at time zone 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
            "AS last_heartbeat "
            "FROM agents "
            "WHERE ($1::agent_status IS NULL OR status = $1::agent_status) "
            "ORDER BY agent_id "
            "LIMIT $2 OFFSET $3",
            status_param,
            limit,
            offset);
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
            record.status = AgentStatusFromApi(row["status"].c_str()).value_or(AgentStatus::Idle);
            record.last_heartbeat = row["last_heartbeat"].c_str();
            agents.push_back(std::move(record));
        }
        return agents;
    });
}

std::int64_t PgBroker::CreateTask(const TaskInput& task) {
    return ExecuteWithRetry("postgres create_task", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);

        std::string args_json = task.args.is_null() ? "[]" : task.args.dump();
        std::string env_json = task.env.is_null() ? "{}" : task.env.dump();

        std::string timeout_text;
        const char* timeout_param = nullptr;
        if (task.timeout_sec) {
            timeout_text = std::to_string(*task.timeout_sec);
            timeout_param = timeout_text.c_str();
        }

        json constraints = NormalizeConstraints(task.constraints);
        std::string constraints_json = constraints.dump();

        auto inserted = tx.exec_params(
            "INSERT INTO tasks (state, command, args, env, timeout_sec, "
            "constraints, assigned_agent, created_at) VALUES "
            "('Queued', $1, $2::jsonb, $3::jsonb, $4::int, $5::jsonb, NULL, NOW()) "
            "RETURNING task_id",
            task.command,
            args_json,
            env_json,
            timeout_param,
            constraints_json);

        std::int64_t task_id = inserted[0]["task_id"].as<std::int64_t>();

        tx.commit();
        spdlog::debug("DB task created: {}", task_id);
        return task_id;
    });
}

std::optional<TaskRecord> PgBroker::GetTask(std::int64_t task_id) {
    return ExecuteWithRetry("postgres get_task", [&]() {
        pqxx::connection conn(connectionString_);
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
            "t.exit_code, t.error_message, t.constraints::text AS constraints "
            "FROM tasks t "
            "WHERE t.task_id = $1",
            task_id);
        if (result.empty()) {
            return std::optional<TaskRecord>{std::nullopt};
        }

        const auto& row = result[0];
        TaskRecord record;
        record.task_id = row["task_id"].as<std::int64_t>();
        record.state = TaskStateFromApi(row["state"].c_str()).value_or(TaskState::Queued);
        record.command = row["command"].c_str();
        record.args = ParseJsonOrDefault(row["args"].c_str(), json::array());
        record.env = ParseJsonOrDefault(row["env"].c_str(), json::object());
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

        record.constraints = BuildConstraintsFromRow(row);
        return std::optional<TaskRecord>{record};
    });
}

std::vector<TaskSummary> PgBroker::ListTasks(const std::optional<TaskState>& state,
                                            const std::optional<std::string>& agent_id,
                                            int limit,
                                            int offset) {
    return ExecuteWithRetry("postgres list_tasks", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);

        std::string db_state;
        const char* state_param = nullptr;
        if (state) {
            db_state = TaskStateToDb(*state);
            state_param = db_state.c_str();
        }
        const char* agent_param = agent_id ? agent_id->c_str() : nullptr;

        auto result = tx.exec_params(
            "SELECT task_id, state::text FROM tasks "
            "WHERE ($1::task_state IS NULL OR state = $1::task_state) "
            "AND ($2::text IS NULL OR assigned_agent = $2) "
            "ORDER BY created_at DESC "
            "LIMIT $3 OFFSET $4",
            state_param,
            agent_param,
            limit,
            offset);
        std::vector<TaskSummary> tasks;
        tasks.reserve(result.size());
        for (const auto& row : result) {
            TaskSummary summary;
            summary.task_id = row["task_id"].as<std::int64_t>();
            summary.state = TaskStateFromApi(row["state"].c_str()).value_or(TaskState::Queued);
            tasks.push_back(std::move(summary));
        }
        return tasks;
    });
}

std::optional<std::vector<TaskDispatch>> PgBroker::PollTasksForAgent(
    const std::string& agent_id,
    int free_slots) {
    return ExecuteWithRetry("postgres poll_tasks_for_agent", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);

        auto agent_result = tx.exec_params(
            "SELECT os, resources_cpu_cores, resources_ram_mb FROM agents WHERE agent_id = $1",
            agent_id);
        if (agent_result.empty()) {
            return std::optional<std::vector<TaskDispatch>>{std::nullopt};
        }

        const auto& agent_row = agent_result[0];
        std::string agent_os = agent_row["os"].c_str();
        int agent_cpu = agent_row["resources_cpu_cores"].as<int>();
        int agent_ram = agent_row["resources_ram_mb"].as<int>();

        tx.exec_params("UPDATE agents SET last_heartbeat = NOW() WHERE agent_id = $1", agent_id);

        if (free_slots <= 0) {
            tx.commit();
            return std::optional<std::vector<TaskDispatch>>{std::vector<TaskDispatch>{}};
        }

        // FIFO scheduling with OS/CPU/RAM filters; SKIP LOCKED avoids double assignment.
        auto tasks_result = tx.exec_params(
            "SELECT t.task_id, t.command, t.args::text, t.env::text, t.timeout_sec, "
            "t.constraints::text AS constraints "
            "FROM tasks t "
            "WHERE t.state = 'Queued' AND t.assigned_agent IS NULL "
            "AND (t.constraints->>'os' IS NULL OR t.constraints->>'os' = $1) "
            "AND (t.constraints->>'cpu_cores' IS NULL OR "
            "(t.constraints->>'cpu_cores')::int <= $2) "
            "AND (t.constraints->>'ram_mb' IS NULL OR "
            "(t.constraints->>'ram_mb')::int <= $3) "
            "ORDER BY t.created_at "
            "FOR UPDATE OF t SKIP LOCKED "
            "LIMIT $4",
            agent_os,
            agent_cpu,
            agent_ram,
            free_slots);

        std::vector<TaskDispatch> dispatches;
        dispatches.reserve(tasks_result.size());

        for (const auto& row : tasks_result) {
            TaskDispatch dispatch;
            dispatch.task_id = row["task_id"].as<std::int64_t>();
            dispatch.command = row["command"].c_str();
            dispatch.args = ParseJsonOrDefault(row["args"].c_str(), json::array());
            dispatch.env = ParseJsonOrDefault(row["env"].c_str(), json::object());
            if (!row["timeout_sec"].is_null()) {
                dispatch.timeout_sec = row["timeout_sec"].as<int>();
            }

            dispatch.constraints = BuildConstraintsFromRow(row);

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
            tx.exec_params("UPDATE agents SET status = $1 WHERE agent_id = $2",
                           AgentStatusToDb(AgentStatus::Busy),
                           agent_id);
        } else {
            tx.exec_params("UPDATE agents SET status = $1 WHERE agent_id = $2",
                           AgentStatusToDb(AgentStatus::Idle),
                           agent_id);
        }

        tx.commit();
        return std::optional<std::vector<TaskDispatch>>{dispatches};
    });
}

bool PgBroker::UpdateTaskStatus(std::int64_t task_id,
                               TaskState state,
                               const std::optional<int>& exit_code,
                               const std::optional<std::string>& started_at,
                               const std::optional<std::string>& finished_at,
                               const std::optional<std::string>& error_message) {
    return ExecuteWithRetry("postgres update_task_status", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);

        std::string db_state = TaskStateToDb(state);

        bool set_started_now = !started_at && state == TaskState::Running;
        bool set_finished_now =
            !finished_at && (state == TaskState::Succeeded || state == TaskState::Failed ||
                             state == TaskState::Canceled);

        std::string exit_code_text;
        const char* exit_code_param = nullptr;
        if (exit_code) {
            exit_code_text = std::to_string(*exit_code);
            exit_code_param = exit_code_text.c_str();
        }

        const char* error_param = error_message ? error_message->c_str() : nullptr;

        std::string started_text;
        const char* started_param = nullptr;
        if (started_at) {
            started_text = *started_at;
            started_param = started_text.c_str();
        }

        std::string finished_text;
        const char* finished_param = nullptr;
        if (finished_at) {
            finished_text = *finished_at;
            finished_param = finished_text.c_str();
        }

        auto result = tx.exec_params(
            "UPDATE tasks SET state = $1, "
            "exit_code = COALESCE($2::int, exit_code), "
            "started_at = CASE "
            "WHEN $3::timestamptz IS NOT NULL THEN $3::timestamptz "
            "WHEN $4::boolean THEN NOW() "
            "ELSE started_at END, "
            "finished_at = CASE "
            "WHEN $5::timestamptz IS NOT NULL THEN $5::timestamptz "
            "WHEN $6::boolean THEN NOW() "
            "ELSE finished_at END, "
            "error_message = COALESCE($7::text, error_message) "
            "WHERE task_id = $8",
            db_state,
            exit_code_param,
            started_param,
            set_started_now,
            finished_param,
            set_finished_now,
            error_param,
            task_id);
        if (result.affected_rows() == 0) {
            return false;
        }

        if (state == TaskState::Succeeded || state == TaskState::Failed ||
            state == TaskState::Canceled) {
            std::string reason = (state == TaskState::Canceled) ? "canceled" : "completed";
            tx.exec_params(
                "UPDATE task_assignments SET unassigned_at = CASE "
                "WHEN $1::timestamptz IS NOT NULL THEN $1::timestamptz "
                "WHEN $2::boolean THEN NOW() "
                "ELSE unassigned_at END, "
                "reason = $3 "
                "WHERE task_id = $4 AND unassigned_at IS NULL",
                finished_param,
                set_finished_now,
                reason,
                task_id);
        }

        tx.commit();
        spdlog::debug("DB task status updated: {} -> {}", task_id, db_state);
        return true;
    });
}

CancelTaskResult PgBroker::CancelTask(std::int64_t task_id) {
    return ExecuteWithRetry("postgres cancel_task", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);

        auto result = tx.exec_params(
            "SELECT state::text FROM tasks WHERE task_id = $1",
            task_id);
        if (result.empty()) {
            return CancelTaskResult::NotFound;
        }

        auto state = TaskStateFromApi(result[0]["state"].c_str()).value_or(TaskState::Queued);
        if (state == TaskState::Succeeded || state == TaskState::Failed ||
            state == TaskState::Canceled) {
            return CancelTaskResult::InvalidState;
        }

        tx.exec_params(
            "UPDATE tasks SET state = $1, finished_at = NOW(), "
            "error_message = COALESCE(error_message, $2) "
            "WHERE task_id = $3",
            TaskStateToDb(TaskState::Canceled),
            "canceled_by_user",
            task_id);

        tx.exec_params(
            "UPDATE task_assignments SET unassigned_at = NOW(), reason = 'canceled_by_user' "
            "WHERE task_id = $1 AND unassigned_at IS NULL",
            task_id);

        tx.commit();
        spdlog::debug("DB task canceled: {}", task_id);
        return CancelTaskResult::Ok;
    });
}

int PgBroker::MarkOfflineAgentsAndRequeue(int offline_after_sec) {
    return ExecuteWithRetry("postgres mark_offline_agents_and_requeue", [&]() {
        pqxx::connection conn(connectionString_);
        pqxx::work tx(conn);

        // Agents past the heartbeat threshold are marked offline, and their tasks are requeued.
        auto agents = tx.exec_params(
            "SELECT agent_id FROM agents "
            "WHERE status <> 'Offline' AND last_heartbeat < "
            "(NOW() - ($1::int * interval '1 second')) "
            "FOR UPDATE",
            offline_after_sec);

        if (agents.empty()) {
            return 0;
        }

        for (const auto& row : agents) {
            std::string agent_id = row["agent_id"].c_str();
            tx.exec_params(
                "UPDATE agents SET status = $1 WHERE agent_id = $2",
                AgentStatusToDb(AgentStatus::Offline),
                agent_id);

            tx.exec_params(
                "UPDATE task_assignments SET unassigned_at = NOW(), reason = 'agent_offline' "
                "WHERE agent_id = $1 AND unassigned_at IS NULL",
                agent_id);

            tx.exec_params(
                "UPDATE tasks SET state = 'Queued', assigned_agent = NULL, started_at = NULL, "
                "error_message = COALESCE(error_message, 'agent_offline_requeued') "
                "WHERE assigned_agent = $1 AND state IN ('Running', 'Queued')",
                agent_id);
        }

        tx.commit();
        spdlog::debug("DB marked offline agents: {}", agents.size());
        return static_cast<int>(agents.size());
    });
}

}  // namespace broker
}  // namespace dc
