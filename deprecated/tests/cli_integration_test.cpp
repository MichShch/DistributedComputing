#include "cli/main.cpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

struct CliTestConfig {
    std::string db_host;
    std::string db_port;
    std::string db_user;
    std::string db_password;
    std::string db_name;
    std::string db_sslmode;
    std::string master_bin;
    std::string master_host = "127.0.0.1";
    int master_port = 0;
    std::string log_dir;
    std::string work_dir;
    int heartbeat_sec = 1;
    int offline_sec = 1;
};

std::string GetEnvVar(const char* key) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : std::string();
}

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

std::string GetRequired(const std::map<std::string, std::string>& data,
                        const std::string& key) {
    auto it = data.find(key);
    if (it == data.end() || it->second.empty()) {
        throw std::runtime_error("Missing required config: " + key);
    }
    return it->second;
}

#if !defined(_WIN32)
int FindFreePort() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Failed to create socket");
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(sock);
        throw std::runtime_error("Failed to bind socket");
    }
    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(sock);
        throw std::runtime_error("Failed to getsockname");
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
#if !defined(_WIN32)
    auto pid = static_cast<long long>(::getpid());
#else
    auto pid = 0LL;
#endif
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = base / ("cli_" + std::to_string(pid) + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

std::optional<std::string> ExtractNumericToken(const std::string& text) {
    std::string token;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            token.push_back(c);
        } else if (!token.empty()) {
            return token;
        }
    }
    if (!token.empty()) {
        return token;
    }
    return std::nullopt;
}

class DbHelper {
public:
    explicit DbHelper(const CliTestConfig& config) : conn_(BuildConnectionString(config)) {}

    void ClearAll() {
        pqxx::work tx(conn_);
        tx.exec("TRUNCATE task_assignments, tasks, agents RESTART IDENTITY CASCADE");
        tx.commit();
    }

    std::string GetTaskStateLower(const std::string& task_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params("SELECT state::text FROM tasks WHERE task_id = $1", task_id);
        tx.commit();
        if (result.empty()) {
            return "";
        }
        std::string value = result[0][0].c_str();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string GetAgentStatusLower(const std::string& agent_id) {
        pqxx::work tx(conn_);
        auto result = tx.exec_params("SELECT status::text FROM agents WHERE agent_id = $1", agent_id);
        tx.commit();
        if (result.empty()) {
            return "";
        }
        std::string value = result[0][0].c_str();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

private:
    static std::string BuildConnectionString(const CliTestConfig& config) {
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

    pqxx::connection conn_;
};

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
        throw std::runtime_error("Master process launch is not implemented on Windows");
#else
        pid_ = ::fork();
        if (pid_ < 0) {
            throw std::runtime_error("Failed to fork master process");
        }
        if (pid_ == 0) {
            if (!work_dir_.empty()) {
                ::chdir(work_dir_.c_str());
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
        int status = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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

bool WaitForMasterReady(const std::string& base_url, std::chrono::seconds timeout) {
    httplib::Client client(base_url);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(1, 0);
    client.set_write_timeout(1, 0);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto res = client.Get("/api/v1/tasks")) {
            if (res->status >= 200 && res->status < 500) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool RegisterAgent(const std::string& base_url, const std::string& agent_id) {
    httplib::Client client(base_url);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    client.set_write_timeout(2, 0);
    json body = {
        {"os", "linux"},
        {"version", "1.0"},
        {"resources", {{"cpu_cores", 2}, {"ram_mb", 256}, {"slots", 1}}}
    };
    auto res = client.Put(("/api/v1/agents/" + agent_id).c_str(),
                          body.dump(),
                          "application/json");
    return res && res->status >= 200 && res->status < 300;
}

std::string MakeId(const std::string& prefix) {
    static std::atomic<int> counter{0};
    return prefix + "_" + std::to_string(counter.fetch_add(1));
}

class CliIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
#if defined(_WIN32)
        skip_ = true;
        GTEST_SKIP() << "CLI integration test not supported on Windows.";
#else
        try {
            LoadConfig();
            db_ = std::make_unique<DbHelper>(config_);
            db_->ClearAll();
            StartMaster();
        } catch (const std::exception& ex) {
            skip_ = true;
            GTEST_SKIP() << ex.what();
        }
#endif
    }

    void TearDown() override {
#if !defined(_WIN32)
        if (master_) {
            master_->Stop();
        }
#endif
    }

    dc::cli::GlobalOptions MakeOptions() const {
        dc::cli::GlobalOptions options;
        options.base_url = base_url_;
        options.timeout_ms = 4000;
        return options;
    }

    dc::cli::ApiClient MakeClient() const {
        return dc::cli::ApiClient(base_url_, 4000);
    }

    CliTestConfig config_;
    std::unique_ptr<DbHelper> db_;
    std::unique_ptr<MasterProcess> master_;
    std::string base_url_;
    bool skip_ = false;

private:
    void LoadConfig() {
        std::string env_path = GetEnvVar("DC_TEST_ENV_FILE");
        if (env_path.empty()) {
            throw std::runtime_error("DC_TEST_ENV_FILE is not set");
        }
        auto env = LoadEnvFile(env_path);
        config_.db_host = GetRequired(env, "DC_TEST_DB_HOST");
        config_.db_port = GetRequired(env, "DC_TEST_DB_PORT");
        config_.db_user = GetRequired(env, "DC_TEST_DB_USER");
        config_.db_password = GetRequired(env, "DC_TEST_DB_PASSWORD");
        config_.db_name = GetRequired(env, "DC_TEST_DB_NAME");
        auto ssl_it = env.find("DC_TEST_DB_SSLMODE");
        if (ssl_it != env.end()) {
            config_.db_sslmode = ssl_it->second;
        }
        std::string port_env = GetEnvVar("DC_TEST_MASTER_PORT");
        if (port_env.empty()) {
#if !defined(_WIN32)
            config_.master_port = FindFreePort();
#else
            config_.master_port = 0;
#endif
        } else {
            config_.master_port = std::stoi(port_env);
        }
        config_.master_bin = GetEnvVar("DC_MASTER_BIN");
        if (config_.master_bin.empty()) {
            throw std::runtime_error("DC_MASTER_BIN is not set");
        }
        config_.log_dir = "test_logs";
        config_.work_dir = MakeTempDir();
        base_url_ = "http://" + config_.master_host + ":" + std::to_string(config_.master_port);
    }

    void StartMaster() {
        std::map<std::string, std::string> env;
        env["DB_HOST"] = config_.db_host;
        env["DB_PORT"] = config_.db_port;
        env["DB_USER"] = config_.db_user;
        env["DB_PASSWORD"] = config_.db_password;
        env["DB_NAME"] = config_.db_name;
        env["LOG_DIR"] = config_.log_dir;
        env["MASTER_HOST"] = config_.master_host;
        env["MASTER_PORT"] = std::to_string(config_.master_port);
        env["HEARTBEAT_SEC"] = std::to_string(config_.heartbeat_sec);
        env["OFFLINE_SEC"] = std::to_string(config_.offline_sec);
        master_ = std::make_unique<MasterProcess>(config_.master_bin, config_.work_dir, env);
        master_->Start();
        if (!WaitForMasterReady(base_url_, std::chrono::seconds(5))) {
            throw std::runtime_error("Master did not become ready");
        }
    }
};

TEST_F(CliIntegrationTest, TasksSubmitAndListAgainstMaster) {
    if (skip_) {
        GTEST_SKIP();
    }

    auto client = MakeClient();
    auto options = MakeOptions();
    std::vector<std::string> submit_args = {"--cmd", "/bin/true"};

    testing::internal::CaptureStdout();
    int submit_code = dc::cli::HandleTasksSubmit(client, options, submit_args);
    std::string submit_out = testing::internal::GetCapturedStdout();

    EXPECT_EQ(submit_code, 0);
    auto submitted_task_id = ExtractNumericToken(submit_out);
    ASSERT_TRUE(submitted_task_id.has_value());
    const std::string task_id = *submitted_task_id;
    EXPECT_EQ(db_->GetTaskStateLower(task_id), "queued");

    testing::internal::CaptureStdout();
    int get_code = dc::cli::HandleTasksGet(client, options, {task_id});
    std::string get_out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(get_code, 0);
    EXPECT_NE(get_out.find(task_id), std::string::npos);

    testing::internal::CaptureStdout();
    int list_code = dc::cli::HandleTasksList(client, options, {});
    std::string list_out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(list_code, 0);
    EXPECT_NE(list_out.find(task_id), std::string::npos);
}

TEST_F(CliIntegrationTest, AgentsListReflectsRegisteredAgent) {
    if (skip_) {
        GTEST_SKIP();
    }

    auto client = MakeClient();
    auto options = MakeOptions();
    std::string agent_id = MakeId("agent");
    ASSERT_TRUE(RegisterAgent(base_url_, agent_id));

    testing::internal::CaptureStdout();
    int code = dc::cli::HandleAgentsList(client, options, {});
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(code, 0);
    EXPECT_NE(output.find(agent_id), std::string::npos);
    EXPECT_EQ(db_->GetAgentStatusLower(agent_id), "idle");
}

}  // namespace
