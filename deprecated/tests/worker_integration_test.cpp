#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
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
    std::string worker_bin;
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
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
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
    std::filesystem::path dir = base / ("worker_" + std::to_string(pid) + "_" +
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
        FailFast("Integration tests require POSIX process control");
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

    ~MasterProcess() { Stop(); }

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
        tx.exec("TRUNCATE task_assignments, tasks, agents RESTART IDENTITY CASCADE");
        tx.commit();
    }

    bool HasAgent(const std::string& agent_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params("SELECT 1 FROM agents WHERE agent_id = $1", agent_id);
        tx.commit();
        return !result.empty();
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
    config.worker_bin = GetEnvVar("DC_WORKER_BIN");
    if (config.worker_bin.empty()) {
        FailFast("Missing required env: DC_WORKER_BIN");
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

class WorkerTestEnvironment : public ::testing::Environment {
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

class WorkerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& state = GetState();
        state.db->ClearAll();
        ClearLogDir(state.config.log_dir);
    }
};

void ExpectStatus(const httplib::Result& res, int status) {
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, status);
}

int RunWorkerOnce(const TestConfig& config,
                  const std::map<std::string, std::string>& env_vars) {
#if defined(_WIN32)
    (void)config;
    (void)env_vars;
    FailFast("Integration tests require POSIX process control");
#else
    pid_t pid = ::fork();
    if (pid < 0) {
        FailFast("Failed to fork worker process");
    }
    if (pid == 0) {
        if (!config.work_dir.empty()) {
            if (::chdir(config.work_dir.c_str()) != 0) {
                std::cerr << "Failed to chdir to " << config.work_dir << std::endl;
                std::exit(127);
            }
        }
        for (const auto& kv : env_vars) {
            ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
        }
        ::execl(config.worker_bin.c_str(), config.worker_bin.c_str(), "--once",
                static_cast<char*>(nullptr));
        std::exit(127);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        FailFast("Failed to wait for worker process");
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

TEST_F(WorkerIntegrationTest, RunsTaskAndReportsSuccess) {
    auto& state = GetState();
    auto client = MakeClient(state.config);

    const std::string agent_id = MakeId("agent");
    json task_payload = {
        {"command", "/bin/sh"},
        {"args", json::array({"-c", "echo worker ok"})},
    };

    auto create_res = client->Post("/api/v1/tasks", task_payload.dump(), "application/json");
    ExpectStatus(create_res, 201);
    auto create_body = json::parse(create_res->body);
    const std::string task_id = std::to_string(create_body["task_id"].get<std::int64_t>());

    std::map<std::string, std::string> worker_env;
    worker_env["MASTER_URL"] = "http://" + state.config.master_host + ":" +
                               std::to_string(state.config.master_port);
    worker_env["AGENT_ID"] = agent_id;
    worker_env["AGENT_OS"] = "linux";
    worker_env["AGENT_VERSION"] = "1.0";
    worker_env["CPU_CORES"] = "2";
    worker_env["RAM_MB"] = "256";
    worker_env["SLOTS"] = "1";
    worker_env["WORKER_LOG_DIR"] = state.config.log_dir + "/worker";
    worker_env["WORKER_LOG_FILE"] = worker_env["WORKER_LOG_DIR"] + "/worker.log";
    worker_env["WORKER_LOG_LEVEL"] = "debug";
    worker_env["CANCEL_CHECK_SEC"] = "1";

    int rc = RunWorkerOnce(state.config, worker_env);
    ASSERT_EQ(rc, 0);

    bool task_done = WaitForCondition(
        [&]() {
            return state.db->GetTaskStateLower(task_id) == "succeeded";
        },
        std::chrono::seconds(10));
    ASSERT_TRUE(task_done);

    EXPECT_TRUE(state.db->HasAgent(agent_id));
    EXPECT_EQ(state.db->GetTaskAssignedAgent(task_id).value_or(""), agent_id);
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new WorkerTestEnvironment());
    return RUN_ALL_TESTS();
}
