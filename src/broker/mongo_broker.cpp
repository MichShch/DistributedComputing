#include "mongo_broker.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <string>
#include <format>
#include <vector>

#include <bsoncxx/v_noabi/bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/v_noabi/bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/v_noabi/bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/v_noabi/bsoncxx/json.hpp>
#include <mongocxx/v_noabi/mongocxx/exception/exception.hpp>
#include <mongocxx/v_noabi/mongocxx/instance.hpp>
#include <mongocxx/v_noabi/mongocxx/options/find.hpp>
#include <mongocxx/v_noabi/mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/v_noabi/mongocxx/pool.hpp>
#include <mongocxx/v_noabi/mongocxx/options/update.hpp>

#include "common/logging.hpp"

namespace dc {
namespace broker {

using bsoncxx::builder::basic::array;
using bsoncxx::builder::basic::document;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using json = nlohmann::json;

namespace {

mongocxx::instance& MongoInstance() {
    static mongocxx::instance instance{};
    return instance;
}

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

std::string DateToIsoUtc(const bsoncxx::types::b_date& date) {
    using clock = std::chrono::system_clock;
    clock::time_point tp(date.value);
    std::time_t t = clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::optional<bsoncxx::types::b_date> ParseIsoUtc(const std::string& raw) {
    if (raw.empty()) {
        return std::nullopt;
    }
    std::tm tm{};
    std::istringstream in(raw);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (in.fail()) {
        return std::nullopt;
    }
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    if (t < 0) {
        return std::nullopt;
    }
    return bsoncxx::types::b_date(std::chrono::system_clock::from_time_t(t));
}

std::optional<std::string> ReadString(const bsoncxx::document::view& doc, const char* key) {
    auto el = doc[key];
    if (!el || el.type() != bsoncxx::type::k_string) {
        return std::nullopt;
    }
    return std::string(el.get_string().value);
}

std::optional<int> ReadInt(const bsoncxx::document::view& doc, const char* key) {
    auto el = doc[key];
    if (!el) {
        return std::nullopt;
    }
    if (el.type() == bsoncxx::type::k_int32) {
        return el.get_int32().value;
    }
    if (el.type() == bsoncxx::type::k_int64) {
        return static_cast<int>(el.get_int64().value);
    }
    return std::nullopt;
}

std::optional<std::int64_t> ReadInt64(const bsoncxx::document::view& doc, const char* key) {
    auto el = doc[key];
    if (!el) {
        return std::nullopt;
    }
    if (el.type() == bsoncxx::type::k_int64) {
        return el.get_int64().value;
    }
    if (el.type() == bsoncxx::type::k_int32) {
        return static_cast<std::int64_t>(el.get_int32().value);
    }
    return std::nullopt;
}

std::optional<bsoncxx::types::b_date> ReadDate(const bsoncxx::document::view& doc, const char* key) {
    auto el = doc[key];
    if (!el || el.type() != bsoncxx::type::k_date) {
        return std::nullopt;
    }
    return bsoncxx::types::b_date(el.get_date().value);
}

bsoncxx::document::value JsonDoc(const json& source) {
    return bsoncxx::from_json(source.dump());
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsTransactionUnsupported(const mongocxx::exception& ex) {
    const std::string message = Lowercase(ex.what());
    return message.find("transaction numbers are only allowed on a replica set member or mongos") !=
               std::string::npos ||
           message.find("transaction numbers are only allowed on a replica set member") !=
               std::string::npos ||
           message.find("transactions are not supported") != std::string::npos ||
           message.find("transaction support") != std::string::npos;
}

}  // namespace

MongoBroker::MongoBroker(DbConfig& config)
    : Broker(config, BrokerType::MONGO, GenerateConnectionString(config)),
      mongo_instance_(MongoInstance()),
      client_pool_(mongocxx::uri(connectionString_)) {
    (void)mongo_instance_;
    ExecuteWithRetry("mongo startup ping", [this]() {
        auto client = client_pool_.acquire();
        auto db = (*client)[config_.dbname];
        db.run_command(make_document(kvp("ping", 1)));
    });
}

std::string MongoBroker::GenerateConnectionString(const DbConfig& config) const {
    switch(config.authMethod) {
        case AuthentificationMethod::PASSWORD:
            return std::format(
                "mongodb://{}:{}@{}:{}/?authSource={}&serverSelectionTimeoutMS={}&connectTimeoutMS={}",
                config.user, config.password, config.host, config.port, config.mongo_auth_source,
                2000, 2000
            );
        case AuthentificationMethod::SSL:
            return std::format(
                "mongodb://{}:{}/?ssl=true&sslclientcertificatekeyfile={}&ssl_key={}&ssl_ca_cert={}&serverSelectionTimeoutMS={}&connectTimeoutMS={}",
                config.host, config.port, config.ssl.cert, config.ssl.key, config.ssl.rootcert,
                2000, 2000
            );
        default:
            throw std::runtime_error("Unexpected case");
    }
}

std::optional<std::int64_t> MongoBroker::NextTaskId(mongocxx::collection& counters,
                                                    mongocxx::client_session& session) {
    mongocxx::options::find_one_and_update opts;
    opts.upsert(true);
    opts.return_document(mongocxx::options::return_document::k_after);
    auto result = counters.find_one_and_update(
        session,
        make_document(kvp("_id", "tasks")),
        make_document(kvp("$inc", make_document(kvp("seq", 1)))),
        opts);
    if (!result) {
        spdlog::error("failed to allocate task_id");
        return std::nullopt;
    }
    auto seq = ReadInt64(result->view(), "seq");
    if (!seq) {
        spdlog::error("counter sequence is missing");
        return std::nullopt;
    }
    return *seq;
}

std::optional<std::int64_t> MongoBroker::NextTaskId(mongocxx::collection& counters) {
    mongocxx::options::find_one_and_update opts;
    opts.upsert(true);
    opts.return_document(mongocxx::options::return_document::k_after);
    auto result = counters.find_one_and_update(
            make_document(kvp("_id", "tasks")),
            make_document(kvp("$inc", make_document(kvp("seq", 1)))),
            opts);
    if (!result) {
        spdlog::error("failed to allocate task_id");
        return std::nullopt;
    }
    auto seq = ReadInt64(result->view(), "seq");
    if (!seq) {
        spdlog::error("counter sequence is missing");
        return std::nullopt;
    }
    return *seq;
}

bool MongoBroker::UpsertAgent(const AgentInput& agent) {
    try {
        return ExecuteWithRetry("mongo upsert_agent", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto agents = db["agents"];
            auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
            auto result = agents.update_one(
                make_document(kvp("agent_id", agent.agent_id)),
                make_document(kvp("$set", make_document(
                    kvp("agent_id", agent.agent_id),
                    kvp("os", agent.os),
                    kvp("version", agent.version),
                    kvp("resources_cpu_cores", agent.cpu_cores),
                    kvp("resources_ram_mb", agent.ram_mb),
                    kvp("resources_slots", agent.slots),
                    kvp("status", AgentStatusToDb(AgentStatus::Idle)),
                    kvp("last_heartbeat", now)))),
                mongocxx::options::update{}.upsert(true));
            return static_cast<bool>(result);
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo upsert_agent failed: {}", ex.what());
        return false;
    } catch (...) {
        spdlog::error("mongo upsert_agent failed with unknown exception.");
        return false;
    }
}

bool MongoBroker::UpdateHeartbeat(const AgentHeartbeat& heartbeat) {
    try {
        return ExecuteWithRetry("mongo update_heartbeat", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto agents = db["agents"];
            auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
            auto result = agents.update_one(
                make_document(kvp("agent_id", heartbeat.agent_id)),
                make_document(kvp("$set", make_document(
                    kvp("status", AgentStatusToDb(heartbeat.status)),
                    kvp("last_heartbeat", now)))));
            return result && result->matched_count() > 0;
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo update_heartbeat failed: {}", ex.what());
        return false;
    } catch (...) {
        spdlog::error("mongo update_heartbeat failed with unknown exception.");
        return false;
    }
}

std::optional<AgentRecord> MongoBroker::GetAgent(const std::string& agent_id) {
    try {
        return ExecuteWithRetry("mongo get_agent", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto agents = db["agents"];
            auto result = agents.find_one(make_document(kvp("agent_id", agent_id)));
            if (!result) {
                return std::optional<AgentRecord>{std::nullopt};
            }
            auto view = result->view();
            AgentRecord record;
            record.agent_id = ReadString(view, "agent_id").value_or("");
            record.os = ReadString(view, "os").value_or("");
            record.version = ReadString(view, "version").value_or("");
            record.cpu_cores = ReadInt(view, "resources_cpu_cores").value_or(0);
            record.ram_mb = ReadInt(view, "resources_ram_mb").value_or(0);
            record.slots = ReadInt(view, "resources_slots").value_or(0);
            record.status = AgentStatusFromApi(ReadString(view, "status").value_or("idle"))
                                .value_or(AgentStatus::Idle);
            record.last_heartbeat = DateToIsoUtc(ReadDate(view, "last_heartbeat").value_or(
                bsoncxx::types::b_date(std::chrono::system_clock::now())));
            return std::optional<AgentRecord>{record};
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo get_agent failed: {}", ex.what());
        return std::nullopt;
    } catch (...) {
        spdlog::error("mongo get_agent failed with unknown exception.");
        return std::nullopt;
    }
}

std::vector<AgentRecord> MongoBroker::ListAgents(const std::optional<AgentStatus>& status,
                                                  int limit,
                                                  int offset) {
    try {
        return ExecuteWithRetry("mongo list_agents", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto agents_collection = db["agents"];
            document filter;
            if (status) {
                filter.append(kvp("status", AgentStatusToDb(*status)));
            }

            mongocxx::options::find opts;
            opts.limit(limit);
            opts.skip(offset);
            opts.sort(make_document(kvp("agent_id", 1)));

            auto cursor = agents_collection.find(filter.view(), opts);
            std::vector<AgentRecord> agents;
            for (const auto& doc : cursor) {
                AgentRecord record;
                record.agent_id = ReadString(doc, "agent_id").value_or("");
                record.os = ReadString(doc, "os").value_or("");
                record.version = ReadString(doc, "version").value_or("");
                record.cpu_cores = ReadInt(doc, "resources_cpu_cores").value_or(0);
                record.ram_mb = ReadInt(doc, "resources_ram_mb").value_or(0);
                record.slots = ReadInt(doc, "resources_slots").value_or(0);
                record.status = AgentStatusFromApi(ReadString(doc, "status").value_or("idle"))
                                    .value_or(AgentStatus::Idle);
                record.last_heartbeat = DateToIsoUtc(ReadDate(doc, "last_heartbeat").value_or(
                    bsoncxx::types::b_date(std::chrono::system_clock::now())));
                agents.push_back(std::move(record));
            }
            return agents;
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo list_agents failed: {}", ex.what());
        return {};
    } catch (...) {
        spdlog::error("mongo list_agents failed with unknown exception.");
        return {};
    }
}

std::int64_t MongoBroker::CreateTask(const TaskInput& task) {
    try {
        return ExecuteWithRetry("mongo create_task", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto tasks = db["tasks"];
            auto counters = db["counters"];
            auto session = client->start_session();
            try {
                session.start_transaction();
                const auto task_id = NextTaskId(counters, session);
                if (!task_id) {
                    try {
                        session.abort_transaction();
                    } catch (...) {
                    }
                    return static_cast<std::int64_t>(0);
                }
            const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
            const json constraints = NormalizeConstraints(task.constraints);

            document task_doc;
            task_doc.append(kvp("task_id", *task_id));
            task_doc.append(kvp("state", TaskStateToDb(TaskState::Queued)));
            task_doc.append(kvp("command", task.command));
            task_doc.append(
                kvp("args_json", (task.args.is_null() ? json::array() : task.args).dump()));
            task_doc.append(
                kvp("env_json", (task.env.is_null() ? json::object() : task.env).dump()));
            task_doc.append(kvp("constraints_json", constraints.dump()));
            task_doc.append(kvp("created_at", now));
            if (task.timeout_sec) {
                task_doc.append(kvp("timeout_sec", *task.timeout_sec));
            }
            if (constraints.contains("os") && constraints["os"].is_string()) {
                task_doc.append(kvp("constraints_os", constraints["os"].get<std::string>()));
            }
            if (constraints.contains("cpu_cores") && constraints["cpu_cores"].is_number_integer()) {
                task_doc.append(kvp("constraints_cpu_cores", constraints["cpu_cores"].get<int>()));
            }
            if (constraints.contains("ram_mb") && constraints["ram_mb"].is_number_integer()) {
                task_doc.append(kvp("constraints_ram_mb", constraints["ram_mb"].get<int>()));
            }

            tasks.insert_one(session, task_doc.view());
            session.commit_transaction();
                spdlog::debug("Mongo task created: {}", *task_id);
                return *task_id;
            } catch (const mongocxx::exception& ex) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                if (IsTransactionUnsupported(ex)) {
                    spdlog::warn(
                        "Mongo transactions unavailable; falling back to non-transactional create.");
                    return CreateTaskNoTransaction(tasks, counters, task);
                }
                throw;
            } catch (...) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                throw;
            }
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo create_task failed: {}", ex.what());
        return 0;
    } catch (...) {
        spdlog::error("mongo create_task failed with unknown exception.");
        return 0;
    }
}

std::int64_t MongoBroker::CreateTaskNoTransaction(mongocxx::collection& tasks,
                                                  mongocxx::collection& counters,
                                                  const TaskInput& task) {
    const auto task_id = NextTaskId(counters);
    if (!task_id) {
        return 0;
    }
    const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
    const json constraints = NormalizeConstraints(task.constraints);

    document task_doc;
    task_doc.append(kvp("task_id", *task_id));
    task_doc.append(kvp("state", TaskStateToDb(TaskState::Queued)));
    task_doc.append(kvp("command", task.command));
    task_doc.append(kvp("args_json", (task.args.is_null() ? json::array() : task.args).dump()));
    task_doc.append(kvp("env_json", (task.env.is_null() ? json::object() : task.env).dump()));
    task_doc.append(kvp("constraints_json", constraints.dump()));
    task_doc.append(kvp("created_at", now));
    if (task.timeout_sec) {
        task_doc.append(kvp("timeout_sec", *task.timeout_sec));
    }
    if (constraints.contains("os") && constraints["os"].is_string()) {
        task_doc.append(kvp("constraints_os", constraints["os"].get<std::string>()));
    }
    if (constraints.contains("cpu_cores") && constraints["cpu_cores"].is_number_integer()) {
        task_doc.append(kvp("constraints_cpu_cores", constraints["cpu_cores"].get<int>()));
    }
    if (constraints.contains("ram_mb") && constraints["ram_mb"].is_number_integer()) {
        task_doc.append(kvp("constraints_ram_mb", constraints["ram_mb"].get<int>()));
    }

    tasks.insert_one(task_doc.view());
    spdlog::debug("Mongo task created without transaction: {}", *task_id);
    return *task_id;
}

std::optional<TaskRecord> MongoBroker::GetTask(std::int64_t task_id) {
    try {
        return ExecuteWithRetry("mongo get_task", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto tasks = db["tasks"];
            auto result = tasks.find_one(make_document(kvp("task_id", task_id)));
            if (!result) {
                return std::optional<TaskRecord>{std::nullopt};
            }

            auto view = result->view();
            TaskRecord record;
            record.task_id = ReadInt64(view, "task_id").value_or(0);
            record.state = TaskStateFromApi(ReadString(view, "state").value_or("queued"))
                               .value_or(TaskState::Queued);
            record.command = ReadString(view, "command").value_or("");
            record.args =
                ParseJsonOrDefault(ReadString(view, "args_json").value_or("[]"), json::array());
            record.env =
                ParseJsonOrDefault(ReadString(view, "env_json").value_or("{}"), json::object());
            record.timeout_sec = ReadInt(view, "timeout_sec");
            record.assigned_agent = ReadString(view, "assigned_agent");

            auto created = ReadDate(view, "created_at");
            if (created) {
                record.created_at = DateToIsoUtc(*created);
            }
            auto started = ReadDate(view, "started_at");
            if (started) {
                record.started_at = DateToIsoUtc(*started);
            }
            auto finished = ReadDate(view, "finished_at");
            if (finished) {
                record.finished_at = DateToIsoUtc(*finished);
            }
            record.exit_code = ReadInt(view, "exit_code");
            record.error_message = ReadString(view, "error_message");
            record.constraints = ParseJsonOrDefault(
                ReadString(view, "constraints_json").value_or("{}"),
                json::object());
            return std::optional<TaskRecord>{record};
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo get_task failed: {}", ex.what());
        return std::nullopt;
    } catch (...) {
        spdlog::error("mongo get_task failed with unknown exception.");
        return std::nullopt;
    }
}

std::vector<TaskSummary> MongoBroker::ListTasks(const std::optional<TaskState>& state,
                                                 const std::optional<std::string>& agent_id,
                                                 int limit,
                                                 int offset) {
    try {
        return ExecuteWithRetry("mongo list_tasks", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto tasks_collection = db["tasks"];
            document filter;
            if (state) {
                filter.append(kvp("state", TaskStateToDb(*state)));
            }
            if (agent_id && !agent_id->empty()) {
                filter.append(kvp("assigned_agent", *agent_id));
            }

            mongocxx::options::find opts;
            opts.limit(limit);
            opts.skip(offset);
            opts.sort(make_document(kvp("created_at", -1)));

            auto cursor = tasks_collection.find(filter.view(), opts);
            std::vector<TaskSummary> tasks;
            for (const auto& doc : cursor) {
                TaskSummary summary;
                summary.task_id = ReadInt64(doc, "task_id").value_or(0);
                summary.state = TaskStateFromApi(ReadString(doc, "state").value_or("queued"))
                                    .value_or(TaskState::Queued);
                tasks.push_back(std::move(summary));
            }
            return tasks;
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo list_tasks failed: {}", ex.what());
        return {};
    } catch (...) {
        spdlog::error("mongo list_tasks failed with unknown exception.");
        return {};
    }
}

std::optional<std::vector<TaskDispatch>> MongoBroker::PollTasksForAgent(const std::string& agent_id,
                                                                         int free_slots) {
    try {
        return ExecuteWithRetry("mongo poll_tasks_for_agent", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto agents = db["agents"];
            auto tasks = db["tasks"];
            auto task_assignments = db["task_assignments"];
            auto session = client->start_session();
            try {
                session.start_transaction();
                auto agent_result = agents.find_one(session, make_document(kvp("agent_id", agent_id)));
                if (!agent_result) {
                    session.abort_transaction();
                    return std::optional<std::vector<TaskDispatch>>{std::nullopt};
                }
                auto agent = agent_result->view();
                const std::string agent_os = ReadString(agent, "os").value_or("");
                const int agent_cpu = ReadInt(agent, "resources_cpu_cores").value_or(0);
                const int agent_ram = ReadInt(agent, "resources_ram_mb").value_or(0);
                const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());

                agents.update_one(
                    session,
                    make_document(kvp("agent_id", agent_id)),
                    make_document(kvp("$set", make_document(kvp("last_heartbeat", now)))));

                if (free_slots <= 0) {
                    session.commit_transaction();
                    return std::optional<std::vector<TaskDispatch>>{std::vector<TaskDispatch>{}};
                }

            json filter = {
                {"state", "Queued"},
                {"$or", json::array({
                    {{"assigned_agent", {{"$exists", false}}}},
                    {{"assigned_agent", nullptr}},
                })},
                {"$and", json::array({
                    {{"$or", json::array({
                        {{"constraints_os", {{"$exists", false}}}},
                        {{"constraints_os", agent_os}},
                    })}},
                    {{"$or", json::array({
                        {{"constraints_cpu_cores", {{"$exists", false}}}},
                        {{"constraints_cpu_cores", {{"$lte", agent_cpu}}}},
                    })}},
                    {{"$or", json::array({
                        {{"constraints_ram_mb", {{"$exists", false}}}},
                        {{"constraints_ram_mb", {{"$lte", agent_ram}}}},
                    })}},
                })},
            };

            mongocxx::options::find find_opts;
            find_opts.sort(make_document(kvp("created_at", 1)));
            find_opts.limit(free_slots);

            auto candidates = tasks.find(session, JsonDoc(filter).view(), find_opts);
            std::vector<TaskDispatch> dispatches;
            for (const auto& candidate : candidates) {
                auto task_id = ReadInt64(candidate, "task_id");
                if (!task_id) {
                    continue;
                }

                json assign_filter = {
                    {"task_id", *task_id},
                    {"state", "Queued"},
                    {"$or", json::array({
                        {{"assigned_agent", {{"$exists", false}}}},
                        {{"assigned_agent", nullptr}},
                    })},
                };
                auto assign_result = tasks.update_one(
                    session,
                    JsonDoc(assign_filter).view(),
                    make_document(kvp("$set", make_document(
                        kvp("state", "Running"),
                        kvp("assigned_agent", agent_id),
                        kvp("started_at", now)))));
                if (!assign_result || assign_result->modified_count() == 0) {
                    continue;
                }

                TaskDispatch dispatch;
                dispatch.task_id = *task_id;
                dispatch.command = ReadString(candidate, "command").value_or("");
                dispatch.args =
                    ParseJsonOrDefault(ReadString(candidate, "args_json").value_or("[]"), json::array());
                dispatch.env =
                    ParseJsonOrDefault(ReadString(candidate, "env_json").value_or("{}"), json::object());
                dispatch.timeout_sec = ReadInt(candidate, "timeout_sec");
                dispatch.constraints = ParseJsonOrDefault(
                    ReadString(candidate, "constraints_json").value_or("{}"),
                    json::object());
                dispatches.push_back(std::move(dispatch));

                task_assignments.insert_one(
                    session,
                    make_document(
                        kvp("task_id", *task_id),
                        kvp("agent_id", agent_id),
                        kvp("assigned_at", now)));
            }

                agents.update_one(
                    session,
                    make_document(kvp("agent_id", agent_id)),
                    make_document(kvp("$set", make_document(
                        kvp("status", dispatches.empty() ? "Idle" : "Busy")))));

                session.commit_transaction();
                return std::optional<std::vector<TaskDispatch>>{dispatches};
            } catch (const mongocxx::exception& ex) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                if (IsTransactionUnsupported(ex)) {
                    spdlog::warn(
                        "Mongo transactions unavailable; falling back to non-transactional poll.");
                    return PollTasksForAgentNoTransaction(
                        agents,
                        tasks,
                        task_assignments,
                        agent_id,
                        free_slots);
                }
                throw;
            } catch (...) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                throw;
            }
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo poll_tasks_for_agent failed: {}", ex.what());
        return std::vector<TaskDispatch>{};
    } catch (...) {
        spdlog::error("mongo poll_tasks_for_agent failed with unknown exception.");
        return std::vector<TaskDispatch>{};
    }
}

std::optional<std::vector<TaskDispatch>> MongoBroker::PollTasksForAgentNoTransaction(
    mongocxx::collection& agents,
    mongocxx::collection& tasks,
    mongocxx::collection& task_assignments,
    const std::string& agent_id,
    int free_slots) {
    auto agent_result = agents.find_one(make_document(kvp("agent_id", agent_id)));
    if (!agent_result) {
        return std::nullopt;
    }
    auto agent = agent_result->view();
    const std::string agent_os = ReadString(agent, "os").value_or("");
    const int agent_cpu = ReadInt(agent, "resources_cpu_cores").value_or(0);
    const int agent_ram = ReadInt(agent, "resources_ram_mb").value_or(0);
    const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());

    agents.update_one(
        make_document(kvp("agent_id", agent_id)),
        make_document(kvp("$set", make_document(kvp("last_heartbeat", now)))));

    if (free_slots <= 0) {
        return std::vector<TaskDispatch>{};
    }

    json filter = {
        {"state", "Queued"},
        {"$or", json::array({
            {{"assigned_agent", {{"$exists", false}}}},
            {{"assigned_agent", nullptr}},
        })},
        {"$and", json::array({
            {{"$or", json::array({
                {{"constraints_os", {{"$exists", false}}}},
                {{"constraints_os", agent_os}},
            })}},
            {{"$or", json::array({
                {{"constraints_cpu_cores", {{"$exists", false}}}},
                {{"constraints_cpu_cores", {{"$lte", agent_cpu}}}},
            })}},
            {{"$or", json::array({
                {{"constraints_ram_mb", {{"$exists", false}}}},
                {{"constraints_ram_mb", {{"$lte", agent_ram}}}},
            })}},
        })},
    };

    mongocxx::options::find find_opts;
    find_opts.sort(make_document(kvp("created_at", 1)));
    find_opts.limit(free_slots);

    auto candidates = tasks.find(JsonDoc(filter).view(), find_opts);
    std::vector<TaskDispatch> dispatches;
    for (const auto& candidate : candidates) {
        auto task_id = ReadInt64(candidate, "task_id");
        if (!task_id) {
            continue;
        }

        json assign_filter = {
            {"task_id", *task_id},
            {"state", "Queued"},
            {"$or", json::array({
                {{"assigned_agent", {{"$exists", false}}}},
                {{"assigned_agent", nullptr}},
            })},
        };
        auto assign_result = tasks.update_one(
            JsonDoc(assign_filter).view(),
            make_document(kvp("$set", make_document(
                kvp("state", "Running"),
                kvp("assigned_agent", agent_id),
                kvp("started_at", now)))));
        if (!assign_result || assign_result->modified_count() == 0) {
            continue;
        }

        TaskDispatch dispatch;
        dispatch.task_id = *task_id;
        dispatch.command = ReadString(candidate, "command").value_or("");
        dispatch.args =
            ParseJsonOrDefault(ReadString(candidate, "args_json").value_or("[]"), json::array());
        dispatch.env =
            ParseJsonOrDefault(ReadString(candidate, "env_json").value_or("{}"), json::object());
        dispatch.timeout_sec = ReadInt(candidate, "timeout_sec");
        dispatch.constraints =
            ParseJsonOrDefault(ReadString(candidate, "constraints_json").value_or("{}"), json::object());
        dispatches.push_back(std::move(dispatch));

        task_assignments.insert_one(make_document(
            kvp("task_id", *task_id),
            kvp("agent_id", agent_id),
            kvp("assigned_at", now)));
    }

    agents.update_one(
        make_document(kvp("agent_id", agent_id)),
        make_document(kvp("$set", make_document(
            kvp("status", dispatches.empty() ? "Idle" : "Busy")))));

    return dispatches;
}

bool MongoBroker::UpdateTaskStatus(std::int64_t task_id,
                                    TaskState state,
                                    const std::optional<int>& exit_code,
                                    const std::optional<std::string>& started_at,
                                    const std::optional<std::string>& finished_at,
                                    const std::optional<std::string>& error_message) {
    try {
        return ExecuteWithRetry("mongo update_task_status", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto tasks = db["tasks"];
            auto task_assignments = db["task_assignments"];
            auto session = client->start_session();
            try {
                session.start_transaction();
                auto current_result = tasks.find_one(session, make_document(kvp("task_id", task_id)));
                if (!current_result) {
                    session.abort_transaction();
                    return false;
                }
                auto current = current_result->view();
                const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());

                document set_doc;
                set_doc.append(kvp("state", TaskStateToDb(state)));
                if (exit_code) {
                    set_doc.append(kvp("exit_code", *exit_code));
                }
                if (error_message && !error_message->empty()) {
                    set_doc.append(kvp("error_message", *error_message));
                }

                bsoncxx::types::b_date finished_for_unassign = now;
                if (started_at) {
                    auto parsed = ParseIsoUtc(*started_at);
                    if (parsed) {
                        set_doc.append(kvp("started_at", *parsed));
                    }
                } else if (state == TaskState::Running && !ReadDate(current, "started_at")) {
                    set_doc.append(kvp("started_at", now));
                }

                const bool terminal =
                    state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled;
                if (finished_at) {
                    auto parsed = ParseIsoUtc(*finished_at);
                    if (parsed) {
                        set_doc.append(kvp("finished_at", *parsed));
                        finished_for_unassign = *parsed;
                    }
                } else if (terminal && !ReadDate(current, "finished_at")) {
                    set_doc.append(kvp("finished_at", now));
                }

                tasks.update_one(
                    session,
                    make_document(kvp("task_id", task_id)),
                    make_document(kvp("$set", set_doc.view())));

                if (terminal) {
                    const std::string reason = (state == TaskState::Canceled) ? "canceled" : "completed";
                    task_assignments.update_many(
                        session,
                        make_document(
                            kvp("task_id", task_id),
                            kvp("unassigned_at", make_document(kvp("$exists", false)))),
                        make_document(kvp("$set", make_document(
                            kvp("unassigned_at", finished_for_unassign),
                            kvp("reason", reason)))));
                }

                session.commit_transaction();
                spdlog::debug("Mongo task status updated: {} -> {}", task_id, TaskStateToDb(state));
                return true;
            } catch (const mongocxx::exception& ex) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                if (IsTransactionUnsupported(ex)) {
                    spdlog::warn(
                        "Mongo transactions unavailable; falling back to non-transactional status update.");
                    return UpdateTaskStatusNoTransaction(
                        tasks,
                        task_assignments,
                        task_id, state, exit_code, started_at, finished_at, error_message);
                }
                throw;
            } catch (...) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                throw;
            }
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo update_task_status failed: {}", ex.what());
        return false;
    } catch (...) {
        spdlog::error("mongo update_task_status failed with unknown exception.");
        return false;
    }
}

bool MongoBroker::UpdateTaskStatusNoTransaction(
    mongocxx::collection& tasks,
    mongocxx::collection& task_assignments,
    std::int64_t task_id,
    TaskState state,
    const std::optional<int>& exit_code,
    const std::optional<std::string>& started_at,
    const std::optional<std::string>& finished_at,
    const std::optional<std::string>& error_message) {
    auto current_result = tasks.find_one(make_document(kvp("task_id", task_id)));
    if (!current_result) {
        return false;
    }
    auto current = current_result->view();
    const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());

    document set_doc;
    set_doc.append(kvp("state", TaskStateToDb(state)));
    if (exit_code) {
        set_doc.append(kvp("exit_code", *exit_code));
    }
    if (error_message && !error_message->empty()) {
        set_doc.append(kvp("error_message", *error_message));
    }

    bsoncxx::types::b_date finished_for_unassign = now;
    if (started_at) {
        auto parsed = ParseIsoUtc(*started_at);
        if (parsed) {
            set_doc.append(kvp("started_at", *parsed));
        }
    } else if (state == TaskState::Running && !ReadDate(current, "started_at")) {
        set_doc.append(kvp("started_at", now));
    }

    const bool terminal =
        state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled;
    if (finished_at) {
        auto parsed = ParseIsoUtc(*finished_at);
        if (parsed) {
            set_doc.append(kvp("finished_at", *parsed));
            finished_for_unassign = *parsed;
        }
    } else if (terminal && !ReadDate(current, "finished_at")) {
        set_doc.append(kvp("finished_at", now));
    }

    tasks.update_one(
        make_document(kvp("task_id", task_id)),
        make_document(kvp("$set", set_doc.view())));

    if (terminal) {
        const std::string reason = (state == TaskState::Canceled) ? "canceled" : "completed";
        task_assignments.update_many(
            make_document(
                kvp("task_id", task_id),
                kvp("unassigned_at", make_document(kvp("$exists", false)))),
            make_document(kvp("$set", make_document(
                kvp("unassigned_at", finished_for_unassign),
                kvp("reason", reason)))));
    }

    spdlog::debug("Mongo task status updated without transaction: {} -> {}",
                  task_id,
                  TaskStateToDb(state));
    return true;
}

CancelTaskResult MongoBroker::CancelTask(std::int64_t task_id) {
    try {
        return ExecuteWithRetry("mongo cancel_task", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto tasks = db["tasks"];
            auto task_assignments = db["task_assignments"];
            auto session = client->start_session();
            try {
                session.start_transaction();
                auto current_result = tasks.find_one(session, make_document(kvp("task_id", task_id)));
                if (!current_result) {
                    session.abort_transaction();
                    return CancelTaskResult::NotFound;
                }
                auto current = current_result->view();
                auto state = TaskStateFromApi(ReadString(current, "state").value_or("queued"))
                                 .value_or(TaskState::Queued);
                if (state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled) {
                    session.abort_transaction();
                    return CancelTaskResult::InvalidState;
                }

                const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
                document set_doc;
                set_doc.append(kvp("state", TaskStateToDb(TaskState::Canceled)));
                set_doc.append(kvp("finished_at", now));
                if (!ReadString(current, "error_message")) {
                    set_doc.append(kvp("error_message", "canceled_by_user"));
                }

                tasks.update_one(
                    session,
                    make_document(kvp("task_id", task_id)),
                    make_document(kvp("$set", set_doc.view())));

                task_assignments.update_many(
                    session,
                    make_document(
                        kvp("task_id", task_id),
                        kvp("unassigned_at", make_document(kvp("$exists", false)))),
                    make_document(kvp("$set", make_document(
                        kvp("unassigned_at", now),
                        kvp("reason", "canceled_by_user")))));

                session.commit_transaction();
                spdlog::debug("Mongo task canceled: {}", task_id);
                return CancelTaskResult::Ok;
            } catch (const mongocxx::exception& ex) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                if (IsTransactionUnsupported(ex)) {
                    spdlog::warn("Mongo transactions unavailable; falling back to non-transactional cancel.");
                    return CancelTaskNoTransaction(tasks, task_assignments, task_id);
                }
                throw;
            } catch (...) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                throw;
            }
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo cancel_task failed: {}", ex.what());
        return CancelTaskResult::Error;
    } catch (...) {
        spdlog::error("mongo cancel_task failed with unknown exception.");
        return CancelTaskResult::Error;
    }
}

CancelTaskResult MongoBroker::CancelTaskNoTransaction(mongocxx::collection& tasks,
                                                      mongocxx::collection& task_assignments,
                                                      std::int64_t task_id) {
    auto current_result = tasks.find_one(make_document(kvp("task_id", task_id)));
    if (!current_result) {
        return CancelTaskResult::NotFound;
    }
    auto current = current_result->view();
    auto state = TaskStateFromApi(ReadString(current, "state").value_or("queued"))
                     .value_or(TaskState::Queued);
    if (state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled) {
        return CancelTaskResult::InvalidState;
    }

    const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
    document set_doc;
    set_doc.append(kvp("state", TaskStateToDb(TaskState::Canceled)));
    set_doc.append(kvp("finished_at", now));
    if (!ReadString(current, "error_message")) {
        set_doc.append(kvp("error_message", "canceled_by_user"));
    }

    tasks.update_one(
        make_document(kvp("task_id", task_id)),
        make_document(kvp("$set", set_doc.view())));

    task_assignments.update_many(
        make_document(
            kvp("task_id", task_id),
            kvp("unassigned_at", make_document(kvp("$exists", false)))),
        make_document(kvp("$set", make_document(
            kvp("unassigned_at", now),
            kvp("reason", "canceled_by_user")))));

    spdlog::debug("Mongo task canceled without transaction: {}", task_id);
    return CancelTaskResult::Ok;
}

int MongoBroker::MarkOfflineAgentsAndRequeue(int offline_after_sec) {
    try {
        return ExecuteWithRetry("mongo mark_offline_agents_and_requeue", [&]() {
            auto client = client_pool_.acquire();
            auto db = (*client)[config_.dbname];
            auto agents = db["agents"];
            auto tasks = db["tasks"];
            auto task_assignments = db["task_assignments"];
            auto session = client->start_session();
            try {
                session.start_transaction();
                const auto cutoff = bsoncxx::types::b_date(
                    std::chrono::system_clock::now() - std::chrono::seconds(offline_after_sec));
                auto candidates = agents.find(
                    session,
                    make_document(
                        kvp("status", make_document(kvp("$ne", "Offline"))),
                        kvp("last_heartbeat", make_document(kvp("$lt", cutoff)))));

                std::vector<std::string> offline_ids;
                for (const auto& doc : candidates) {
                    auto id = ReadString(doc, "agent_id");
                    if (id) {
                        offline_ids.push_back(*id);
                    }
                }

                if (offline_ids.empty()) {
                    session.commit_transaction();
                    return 0;
                }

                const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
                for (const auto& agent_id : offline_ids) {
                    agents.update_one(
                        session,
                        make_document(kvp("agent_id", agent_id)),
                        make_document(kvp("$set", make_document(kvp("status", "Offline")))));

                    task_assignments.update_many(
                        session,
                        make_document(
                            kvp("agent_id", agent_id),
                            kvp("unassigned_at", make_document(kvp("$exists", false)))),
                        make_document(kvp("$set", make_document(
                            kvp("unassigned_at", now),
                            kvp("reason", "agent_offline")))));

                    tasks.update_many(
                        session,
                        make_document(
                            kvp("assigned_agent", agent_id),
                            kvp("state", make_document(kvp("$in", make_array("Running", "Queued"))))),
                        make_document(kvp("$set", make_document(
                            kvp("state", "Queued"),
                            kvp("assigned_agent", bsoncxx::types::b_null{}),
                            kvp("started_at", bsoncxx::types::b_null{}),
                            kvp("error_message", "agent_offline_requeued")))));
                }

                session.commit_transaction();
                spdlog::debug("Mongo marked offline agents: {}", offline_ids.size());
                return static_cast<int>(offline_ids.size());
            } catch (const mongocxx::exception& ex) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                if (IsTransactionUnsupported(ex)) {
                    spdlog::warn(
                        "Mongo transactions unavailable; falling back to non-transactional offline sweep.");
                    return MarkOfflineAgentsAndRequeueNoTransaction(
                        agents,
                        tasks,
                        task_assignments,
                        offline_after_sec);
                }
                throw;
            } catch (...) {
                try {
                    session.abort_transaction();
                } catch (...) {
                }
                throw;
            }
        });
    } catch (const std::exception& ex) {
        spdlog::error("mongo mark_offline_agents_and_requeue failed: {}", ex.what());
        return 0;
    } catch (...) {
        spdlog::error("mongo mark_offline_agents_and_requeue failed with unknown exception.");
        return 0;
    }
}

int MongoBroker::MarkOfflineAgentsAndRequeueNoTransaction(mongocxx::collection& agents,
                                                          mongocxx::collection& tasks,
                                                          mongocxx::collection& task_assignments,
                                                          int offline_after_sec) {
    const auto cutoff = bsoncxx::types::b_date(
        std::chrono::system_clock::now() - std::chrono::seconds(offline_after_sec));
    auto candidates = agents.find(make_document(
        kvp("status", make_document(kvp("$ne", "Offline"))),
        kvp("last_heartbeat", make_document(kvp("$lt", cutoff)))));

    std::vector<std::string> offline_ids;
    for (const auto& doc : candidates) {
        auto id = ReadString(doc, "agent_id");
        if (id) {
            offline_ids.push_back(*id);
        }
    }

    if (offline_ids.empty()) {
        return 0;
    }

    const auto now = bsoncxx::types::b_date(std::chrono::system_clock::now());
    for (const auto& agent_id : offline_ids) {
        agents.update_one(
            make_document(kvp("agent_id", agent_id)),
            make_document(kvp("$set", make_document(kvp("status", "Offline")))));

        task_assignments.update_many(
            make_document(
                kvp("agent_id", agent_id),
                kvp("unassigned_at", make_document(kvp("$exists", false)))),
            make_document(kvp("$set", make_document(
                kvp("unassigned_at", now),
                kvp("reason", "agent_offline")))));

        tasks.update_many(
            make_document(
                kvp("assigned_agent", agent_id),
                kvp("state", make_document(kvp("$in", make_array("Running", "Queued"))))),
            make_document(kvp("$set", make_document(
                kvp("state", "Queued"),
                kvp("assigned_agent", bsoncxx::types::b_null{}),
                kvp("started_at", bsoncxx::types::b_null{}),
                kvp("error_message", "agent_offline_requeued")))));
    }

    spdlog::debug("Mongo marked offline agents without transaction: {}", offline_ids.size());
    return static_cast<int>(offline_ids.size());
}

}  // namespace broker
}  // namespace dc
