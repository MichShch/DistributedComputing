#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

struct TestConfig {
    std::string db_host;
    std::string db_port;
    std::string db_user;
    std::string db_password;
    std::string db_name;
    std::string db_sslmode;
    std::string master_bin;
    std::string master_host;
    int master_port = 0;
    std::string log_dir;
    int heartbeat_sec = 1;
    int offline_sec = 1;
    std::string work_dir;
};

struct TestState {
    TestConfig config;
    std::unique_ptr<class MasterProcess> master;
    std::unique_ptr<class DbHelper> db;
};

std::unique_ptr<TestState> g_state;

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string StripQuotes(const std::string& value) {
    if (value.size() >= 2) {
        char first = value.front();
        char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

std::map<std::string, std::string> LoadEnvFile(const std::string& path) {
    std::map<std::string, std::string> data;
    std::ifstream in(path);
    if (!in.is_open()) {
        return data;
    }
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("export ", 0) == 0) {
            line = Trim(line.substr(7));
        }
        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        std::string key = Trim(line.substr(0, pos));
        std::string val = Trim(line.substr(pos + 1));
        data[key] = StripQuotes(val);
    }
    return data;
}

std::string GetEnvVar(const char* key) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : std::string();
}

[[noreturn]] void FailFast(const std::string& message) {
    std::cerr << message << std::endl;
    std::exit(2);
}

std::string GetRequired(const std::map<std::string, std::string>& data,
                        const std::string& key) {
    auto it = data.find(key);
    if (it == data.end() || it->second.empty()) {
        FailFast("Missing required config: " + key);
    }
    return it->second;
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string BuildConnectionString(const TestConfig& config) {
    std::ostringstream out;
    if (!config.db_host.empty()) {
        out << "host=" << config.db_host << " ";
    }
    if (!config.db_port.empty()) {
        out << "port=" << config.db_port << " ";
    }
    if (!config.db_user.empty()) {
        out << "user=" << config.db_user << " ";
    }
    if (!config.db_password.empty()) {
        out << "password=" << config.db_password << " ";
    }
    if (!config.db_name.empty()) {
        out << "dbname=" << config.db_name << " ";
    }
    if (!config.db_sslmode.empty()) {
        out << "sslmode=" << config.db_sslmode << " ";
    }
    return out.str();
}

#if !defined(_WIN32)
int FindFreePort() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        FailFast("Failed to create socket to find free port");
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(sock);
        FailFast("Failed to bind socket to find free port");
    }
    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(sock);
        FailFast("Failed to get socket name for free port");
    }
    int port = ntohs(addr.sin_port);
    ::close(sock);
    return port;
}
#endif

std::string MakeTempDir() {
    std::filesystem::path base = std::filesystem::current_path() / "test_logs";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if !defined(_WIN32)
    auto pid = static_cast<long long>(::getpid());
#else
    auto pid = static_cast<long long>(0);
#endif
    std::filesystem::path dir = base / ("master_" + std::to_string(pid) + "_" +
                                        std::to_string(stamp));
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

void ClearLogDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
}

std::string MakeId(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + "_" + std::to_string(counter.fetch_add(1));
}

class MasterProcess {
public:
    MasterProcess(std::string binary,
                  std::string work_dir,
                  std::map<std::string, std::string> env)
        : binary_(std::move(binary)),
          work_dir_(std::move(work_dir)),
          env_(std::move(env)) {}

    void Start() {
#if defined(_WIN32)
        FailFast("Master process launch is not implemented on Windows");
#else
        pid_ = ::fork();
        if (pid_ < 0) {
            FailFast("Failed to fork master process");
        }
        if (pid_ == 0) {
            if (!work_dir_.empty()) {
                if (::chdir(work_dir_.c_str()) != 0) {
                    std::cerr << "Failed to chdir to " << work_dir_ << std::endl;
                    std::exit(127);
                }
            }
            for (const auto& kv : env_) {
                ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
            }
            ::execl(binary_.c_str(), binary_.c_str(), static_cast<char*>(nullptr));
            std::exit(127);
        }
#endif
    }

    void Stop() {
#if !defined(_WIN32)
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
#endif
    }

    ~MasterProcess() {
        Stop();
    }

private:
    std::string binary_;
    std::string work_dir_;
    std::map<std::string, std::string> env_;
#if !defined(_WIN32)
    pid_t pid_ = -1;
#endif
};

class DbHelper {
public:
    explicit DbHelper(const TestConfig& config) : conn_(BuildConnectionString(config)) {}

    void ClearAll() {
        pqxx::work tx(conn_);
        tx.exec("TRUNCATE task_assignments, task_constraints, tasks, agents "
                "RESTART IDENTITY CASCADE");
        tx.commit();
    }

    bool HasAgent(const std::string& agent_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params("SELECT 1 FROM agents WHERE agent_id = $1", agent_id);
        tx.commit();
        return !result.empty();
    }

    std::string GetAgentStatusLower(const std::string& agent_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT status::text FROM agents WHERE agent_id = $1", agent_id);
        tx.commit();
        if (result.empty()) {
            return "";
        }
        return ToLowerCopy(result[0][0].c_str());
    }

    std::string GetTaskStateLower(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT state::text FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty()) {
            return "";
        }
        return ToLowerCopy(result[0][0].c_str());
    }

    std::optional<std::string> GetTaskAssignedAgent(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT assigned_agent FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }
        return result[0][0].c_str();
    }

    bool TaskStarted(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT started_at IS NOT NULL FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty()) {
            return false;
        }
        return result[0][0].as<bool>();
    }

    bool TaskFinished(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT finished_at IS NOT NULL FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty()) {
            return false;
        }
        return result[0][0].as<bool>();
    }

    std::optional<int> GetTaskExitCode(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT exit_code FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }
        return result[0][0].as<int>();
    }

    std::optional<std::string> GetTaskErrorMessage(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT error_message FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }
        return result[0][0].c_str();
    }

    int CountAssignments(const std::string& task_id, bool only_open) {
        pqxx::work tx(conn_);
        std::string query =
            "SELECT COUNT(*) FROM task_assignments WHERE task_id = $1";
        if (only_open) {
            query += " AND unassigned_at IS NULL";
        }
        auto result = tx.exec_params(query, task_id);
        tx.commit();
        return result.empty() ? 0 : result[0][0].as<int>();
    }

    std::optional<std::string> GetAssignmentReason(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT reason FROM task_assignments WHERE task_id = $1 "
            "ORDER BY assigned_at DESC LIMIT 1",
            task_id);
        tx.commit();
        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }
        return result[0][0].c_str();
    }

    std::optional<std::string> GetConstraintOs(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT os FROM task_constraints WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }
        return result[0][0].c_str();
    }

    std::optional<int> GetConstraintCpu(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params(
            "SELECT cpu_cores FROM task_constraints WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }
        return result[0][0].as<int>();
    }

    void SetAgentHeartbeatAge(const std::string& agent_id, int seconds_ago) {
        pqxx::work tx(conn_);
        tx.exec_params(
            "UPDATE agents SET last_heartbeat = NOW() - ($2 || ' seconds')::interval "
            "WHERE agent_id = $1",
            agent_id,
            std::to_string(seconds_ago));
        tx.commit();
    }

private:
    pqxx::connection conn_;
};

TestState& GetState() {
    if (!g_state) {
        FailFast("Test state not initialized");
    }
    return *g_state;
}

TestConfig LoadConfig() {
    std::string env_path = GetEnvVar("DC_TEST_ENV_FILE");
    if (env_path.empty()) {
        env_path = "configs/master.env";
    }
    std::filesystem::path env_path_fs = std::filesystem::absolute(env_path);
    auto env = LoadEnvFile(env_path_fs.string());
    if (env.empty()) {
        FailFast("Failed to read env file: " + env_path_fs.string());
    }

    TestConfig config;
    config.db_host = GetRequired(env, "DC_TEST_DB_HOST");
    config.db_port = GetRequired(env, "DC_TEST_DB_PORT");
    config.db_user = GetRequired(env, "DC_TEST_DB_USER");
    config.db_password = GetRequired(env, "DC_TEST_DB_PASSWORD");
    config.db_name = GetRequired(env, "DC_TEST_DB_NAME");
    auto ssl_it = env.find("DC_TEST_DB_SSLMODE");
    if (ssl_it != env.end()) {
        config.db_sslmode = ssl_it->second;
    }

    config.master_bin = GetEnvVar("DC_MASTER_BIN");
    if (config.master_bin.empty()) {
        FailFast("Missing required env: DC_MASTER_BIN");
    }
    if (env_path_fs.has_parent_path()) {
        auto configs_dir = env_path_fs.parent_path();
        if (configs_dir.has_parent_path()) {
            config.work_dir = configs_dir.parent_path().string();
        }
    }
    if (config.work_dir.empty()) {
        config.work_dir = std::filesystem::current_path().string();
    }
    config.master_host = "127.0.0.1";
    auto port_it = env.find("DC_TEST_MASTER_PORT");
    if (port_it != env.end() && !port_it->second.empty()) {
        config.master_port = std::stoi(port_it->second);
    } else if ((port_it = env.find("MASTER_PORT")) != env.end() && !port_it->second.empty()) {
        config.master_port = std::stoi(port_it->second);
    } else {
        config.master_port = 18080;
    }
    config.log_dir = MakeTempDir();
    config.heartbeat_sec = 1;
    config.offline_sec = 1;
    return config;
}

std::unique_ptr<httplib::Client> MakeClient(const TestConfig& config) {
    auto client = std::make_unique<httplib::Client>(config.master_host, config.master_port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(5, 0);
    client->set_write_timeout(5, 0);
    return client;
}

bool WaitForReady(const TestConfig& config, std::chrono::seconds timeout) {
    auto client = MakeClient(config);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto res = client->Get("/api/v1/agents?limit=1");
        if (res && res->status == 200) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool WaitForCondition(const std::function<bool()>& predicate,
                      std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return predicate();
}

class MasterTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
#if defined(_WIN32)
        FailFast("Integration tests require POSIX process control");
#endif
        TestConfig config = LoadConfig();
        std::map<std::string, std::string> child_env;
        child_env["DB_HOST"] = config.db_host;
        child_env["DB_PORT"] = config.db_port;
        child_env["DB_USER"] = config.db_user;
        child_env["DB_PASSWORD"] = config.db_password;
        child_env["DB_NAME"] = config.db_name;
        if (!config.db_sslmode.empty()) {
            child_env["DB_SSLMODE"] = config.db_sslmode;
        }
        child_env["MASTER_HOST"] = config.master_host;
        child_env["MASTER_PORT"] = std::to_string(config.master_port);
        child_env["LOG_DIR"] = config.log_dir;
        child_env["HEARTBEAT_SEC"] = std::to_string(config.heartbeat_sec);
        child_env["OFFLINE_SEC"] = std::to_string(config.offline_sec);

        auto master = std::make_unique<MasterProcess>(config.master_bin,
                                                      config.work_dir,
                                                      child_env);
        master->Start();
        if (!WaitForReady(config, std::chrono::seconds(10))) {
            master->Stop();
            FailFast("Master did not become ready on port " +
                     std::to_string(config.master_port));
        }

        auto db = std::make_unique<DbHelper>(config);
        g_state = std::make_unique<TestState>();
        g_state->config = std::move(config);
        g_state->master = std::move(master);
        g_state->db = std::move(db);
    }

    void TearDown() override {
        if (g_state) {
            if (g_state->master) {
                g_state->master->Stop();
            }
            if (!g_state->config.log_dir.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(g_state->config.log_dir, ec);
            }
            g_state.reset();
        }
    }
};

class MasterIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& state = GetState();
        state.db->ClearAll();
        ClearLogDir(state.config.log_dir);
    }
};

void ExpectErrorCode(const httplib::Result& res, int status,
                     const std::string& code) {
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, status);
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("error"));
    ASSERT_EQ(body["error"]["code"], code);
}

void ExpectStatus(const httplib::Result& res, int status) {
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, status);
}

TEST_F(MasterIntegrationTest, RegisterAgentAndHeartbeat) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string agent_id = MakeId("agent");
    json payload = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 4}, {"ram_mb", 2048}, {"slots", 2}}},
    };

    auto res = client->Put("/api/v1/agents/" + agent_id, payload.dump(),
                           "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_EQ(body["heartbeat_interval_sec"], state.config.heartbeat_sec);
    EXPECT_EQ(state.db->GetAgentStatusLower(agent_id), "idle");

    json heartbeat = {{"status", "busy"}};
    res = client->Post("/api/v1/agents/" + agent_id + "/heartbeat",
                       heartbeat.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(state.db->GetAgentStatusLower(agent_id), "busy");
}

TEST_F(MasterIntegrationTest, RegisterAgentValidation) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string agent_id = MakeId("agent_bad");
    json payload = {{"os", "linux"}};
    auto res = client->Put("/api/v1/agents/" + agent_id, payload.dump(),
                           "application/json");
    ExpectErrorCode(res, 400, "BAD_REQUEST");
    EXPECT_FALSE(state.db->HasAgent(agent_id));
}

TEST_F(MasterIntegrationTest, ListAgentsByStatus) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string idle_id = MakeId("agent_idle");
    std::string busy_id = MakeId("agent_busy");

    json payload = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 2}, {"ram_mb", 512}, {"slots", 1}}},
    };

    ExpectStatus(client->Put("/api/v1/agents/" + idle_id, payload.dump(),
                             "application/json"), 200);
    ExpectStatus(client->Put("/api/v1/agents/" + busy_id, payload.dump(),
                             "application/json"), 200);
    json heartbeat = {{"status", "busy"}};
    ExpectStatus(client->Post("/api/v1/agents/" + busy_id + "/heartbeat",
                              heartbeat.dump(), "application/json"), 200);

    auto res = client->Get("/api/v1/agents?status=busy");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("agents"));
    ASSERT_EQ(body["agents"].size(), 1);
    EXPECT_EQ(body["agents"][0]["agent_id"], busy_id);
}

TEST_F(MasterIntegrationTest, GetAgentDetails) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string agent_id = MakeId("agent_detail");
    json payload = {
        {"os", "linux"},
        {"version", "2.1"},
        {"resources", {{"cpu_cores", 8}, {"ram_mb", 4096}, {"slots", 4}}},
    };
    ExpectStatus(client->Put("/api/v1/agents/" + agent_id, payload.dump(),
                             "application/json"), 200);

    auto res = client->Get("/api/v1/agents/" + agent_id);
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["agent"]["agent_id"], agent_id);
    EXPECT_EQ(body["agent"]["os"], "linux");
    EXPECT_EQ(body["agent"]["version"], "2.1");
    EXPECT_EQ(body["agent"]["resources"]["cpu_cores"], 8);
    EXPECT_EQ(body["agent"]["resources"]["ram_mb"], 4096);
    EXPECT_EQ(body["agent"]["resources"]["slots"], 4);
    EXPECT_EQ(body["agent"]["status"], "idle");
}

TEST_F(MasterIntegrationTest, SubmitTaskAndGet) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string task_id = MakeId("task");
    json payload = {
        {"task_id", task_id},
        {"command", "echo"},
        {"args", json::array({"hello"})},
        {"env", {{"KEY", "VALUE"}}},
        {"timeout_sec", 5},
        {"constraints", {{"os", "linux"}, {"cpu_cores", 1}}},
    };

    auto res = client->Post("/api/v1/tasks", payload.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 201);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["task_id"], task_id);

    EXPECT_EQ(state.db->GetTaskStateLower(task_id), "queued");
    EXPECT_EQ(state.db->GetConstraintOs(task_id).value_or(""), "linux");
    EXPECT_EQ(state.db->GetConstraintCpu(task_id).value_or(0), 1);

    res = client->Get("/api/v1/tasks/" + task_id);
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    body = json::parse(res->body);
    EXPECT_EQ(body["task"]["task_id"], task_id);
    EXPECT_EQ(body["task"]["state"], "queued");

    res = client->Post("/api/v1/tasks", payload.dump(), "application/json");
    ExpectErrorCode(res, 409, "TASK_EXISTS");
}

TEST_F(MasterIntegrationTest, ListTasksByState) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string agent_id = MakeId("agent_list");
    json agent_payload = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 2}, {"ram_mb", 1024}, {"slots", 1}}},
    };
    ExpectStatus(client->Put("/api/v1/agents/" + agent_id, agent_payload.dump(),
                             "application/json"), 200);

    std::string task_id1 = MakeId("task_list");
    std::string task_id2 = MakeId("task_list");
    json payload1 = {{"task_id", task_id1}, {"command", "echo"}};
    json payload2 = {{"task_id", task_id2}, {"command", "echo"}};
    ExpectStatus(client->Post("/api/v1/tasks", payload1.dump(),
                              "application/json"), 201);
    ExpectStatus(client->Post("/api/v1/tasks", payload2.dump(),
                              "application/json"), 201);

    json poll = {{"free_slots", 1}};
    ExpectStatus(client->Post("/api/v1/agents/" + agent_id + "/tasks:poll",
                              poll.dump(), "application/json"), 200);

    auto running = client->Get("/api/v1/tasks?state=running");
    ASSERT_TRUE(running);
    ASSERT_EQ(running->status, 200);
    auto running_body = json::parse(running->body);
    ASSERT_EQ(running_body["tasks"].size(), 1);

    auto queued = client->Get("/api/v1/tasks?state=queued");
    ASSERT_TRUE(queued);
    ASSERT_EQ(queued->status, 200);
    auto queued_body = json::parse(queued->body);
    ASSERT_EQ(queued_body["tasks"].size(), 1);
    EXPECT_NE(running_body["tasks"][0]["task_id"],
              queued_body["tasks"][0]["task_id"]);
}

TEST_F(MasterIntegrationTest, TaskStatusTransitionValidation) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string task_id = MakeId("task_transition");
    json payload = {{"task_id", task_id}, {"command", "echo"}};
    ExpectStatus(client->Post("/api/v1/tasks", payload.dump(), "application/json"), 201);

    json status = {{"state", "succeeded"}, {"exit_code", 0}};
    auto res = client->Post("/api/v1/tasks/" + task_id + "/status",
                            status.dump(), "application/json");
    ExpectErrorCode(res, 409, "INVALID_STATE_TRANSITION");
    EXPECT_EQ(state.db->GetTaskStateLower(task_id), "queued");
}

TEST_F(MasterIntegrationTest, PollAssignsAndCompletesTasks) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string agent_id = MakeId("agent_poll");
    json agent_payload = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 4}, {"ram_mb", 2048}, {"slots", 2}}},
    };
    ExpectStatus(client->Put("/api/v1/agents/" + agent_id, agent_payload.dump(),
                             "application/json"), 200);

    std::string task_id1 = MakeId("task_poll");
    std::string task_id2 = MakeId("task_poll");
    json task_payload1 = {
        {"task_id", task_id1},
        {"command", "echo"},
        {"constraints", {{"os", "linux"}}},
    };
    json task_payload2 = {
        {"task_id", task_id2},
        {"command", "echo"},
        {"constraints", {{"os", "linux"}}},
    };
    ExpectStatus(client->Post("/api/v1/tasks", task_payload1.dump(),
                              "application/json"), 201);
    ExpectStatus(client->Post("/api/v1/tasks", task_payload2.dump(),
                              "application/json"), 201);

    json poll = {{"free_slots", 1}};
    auto res = client->Post("/api/v1/agents/" + agent_id + "/tasks:poll",
                            poll.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    ASSERT_EQ(body["tasks"].size(), 1);
    std::string assigned_task = body["tasks"][0]["task_id"];

    EXPECT_EQ(state.db->GetTaskStateLower(assigned_task), "running");
    EXPECT_EQ(state.db->GetTaskAssignedAgent(assigned_task).value_or(""), agent_id);
    EXPECT_TRUE(state.db->TaskStarted(assigned_task));
    EXPECT_EQ(state.db->CountAssignments(assigned_task, true), 1);

    json status = {{"state", "succeeded"}, {"exit_code", 0}};
    res = client->Post("/api/v1/tasks/" + assigned_task + "/status",
                       status.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(state.db->GetTaskStateLower(assigned_task), "succeeded");
    EXPECT_TRUE(state.db->TaskFinished(assigned_task));
    EXPECT_EQ(state.db->GetTaskExitCode(assigned_task).value_or(-1), 0);
    EXPECT_EQ(state.db->CountAssignments(assigned_task, true), 0);
    EXPECT_EQ(state.db->GetAssignmentReason(assigned_task).value_or(""), "completed");

    res = client->Post("/api/v1/agents/" + agent_id + "/tasks:poll",
                       poll.dump(), "application/json");
    ASSERT_TRUE(res);
    body = json::parse(res->body);
    ASSERT_EQ(body["tasks"].size(), 1);
    EXPECT_NE(body["tasks"][0]["task_id"], assigned_task);
}

TEST_F(MasterIntegrationTest, CancelTask) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string task_id = MakeId("task_cancel");
    json payload = {{"task_id", task_id}, {"command", "echo"}};
    ExpectStatus(client->Post("/api/v1/tasks", payload.dump(), "application/json"), 201);

    auto res = client->Post("/api/v1/tasks/" + task_id + "/cancel", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(state.db->GetTaskStateLower(task_id), "canceled");
    EXPECT_EQ(state.db->GetTaskErrorMessage(task_id).value_or(""), "canceled_by_user");

    std::string agent_id = MakeId("agent_cancel");
    json agent_payload = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 2}, {"ram_mb", 512}, {"slots", 1}}},
    };
    ExpectStatus(client->Put("/api/v1/agents/" + agent_id, agent_payload.dump(),
                             "application/json"), 200);
    std::string task_id2 = MakeId("task_cancel");
    json payload2 = {{"task_id", task_id2}, {"command", "echo"}};
    ExpectStatus(client->Post("/api/v1/tasks", payload2.dump(), "application/json"), 201);
    json poll = {{"free_slots", 1}};
    ExpectStatus(client->Post("/api/v1/agents/" + agent_id + "/tasks:poll",
                              poll.dump(), "application/json"), 200);
    json status = {{"state", "succeeded"}};
    ExpectStatus(client->Post("/api/v1/tasks/" + task_id2 + "/status",
                              status.dump(), "application/json"), 200);

    res = client->Post("/api/v1/tasks/" + task_id2 + "/cancel", "", "application/json");
    ExpectErrorCode(res, 409, "TASK_INVALID_STATE");
}

TEST_F(MasterIntegrationTest, LogsEndpoints) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string task_id = MakeId("task_logs");
    json payload = {{"task_id", task_id}, {"command", "echo"}};
    ExpectStatus(client->Post("/api/v1/tasks", payload.dump(), "application/json"), 201);

    std::filesystem::path task_dir =
        std::filesystem::path(state.config.log_dir) / task_id;
    std::filesystem::create_directories(task_dir);
    std::ofstream stdout_file(task_dir / "stdout.log");
    stdout_file << "hello stdout\n";
    std::ofstream stderr_file(task_dir / "stderr.log");
    stderr_file << "hello stderr\n";
    stdout_file.close();
    stderr_file.close();

    auto res = client->Get("/api/v1/tasks/" + task_id + "/logs?stream=stdout");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    EXPECT_EQ(res->body, "hello stdout\n");

    res = client->Get("/api/v1/tasks/" + task_id + "/logs:tail?stream=stdout&from=6");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    EXPECT_EQ(res->body, "stdout\n");
    auto size_header = res->get_header_value("X-Log-Size");
    EXPECT_FALSE(size_header.empty());

    res = client->Get("/api/v1/tasks/" + task_id + "/logs?stream=invalid");
    ExpectErrorCode(res, 400, "BAD_REQUEST");

    std::filesystem::path meta_path = task_dir / "meta.json";
    EXPECT_TRUE(std::filesystem::exists(meta_path));
}

TEST_F(MasterIntegrationTest, MaintenanceRequeuesOfflineAgent) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    std::string agent_id = MakeId("agent_offline");
    json agent_payload = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 2}, {"ram_mb", 512}, {"slots", 1}}},
    };
    ExpectStatus(client->Put("/api/v1/agents/" + agent_id, agent_payload.dump(),
                             "application/json"), 200);

    std::string task_id = MakeId("task_offline");
    json task_payload = {
        {"task_id", task_id},
        {"command", "echo"},
        {"constraints", {{"os", "linux"}}},
    };
    ExpectStatus(client->Post("/api/v1/tasks", task_payload.dump(),
                              "application/json"), 201);

    json poll = {{"free_slots", 1}};
    ExpectStatus(client->Post("/api/v1/agents/" + agent_id + "/tasks:poll",
                              poll.dump(), "application/json"), 200);
    EXPECT_EQ(state.db->GetTaskStateLower(task_id), "running");

    state.db->SetAgentHeartbeatAge(agent_id, 5);

    bool requeued = WaitForCondition([&]() {
        return state.db->GetAgentStatusLower(agent_id) == "offline" &&
               state.db->GetTaskStateLower(task_id) == "queued";
    }, std::chrono::seconds(15));
    ASSERT_TRUE(requeued);

    EXPECT_FALSE(state.db->GetTaskAssignedAgent(task_id).has_value());
    EXPECT_EQ(state.db->GetTaskErrorMessage(task_id).value_or(""),
              "agent_offline_requeued");
    EXPECT_EQ(state.db->GetAssignmentReason(task_id).value_or(""), "agent_offline");
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new MasterTestEnvironment());
    return RUN_ALL_TESTS();
}
