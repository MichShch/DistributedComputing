#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "common/time_utils.h"
#include "nlohmann/json.hpp"
#include "worker/agent_client.h"
#include "worker/task_executor.h"
#include "worker/worker_app.h"

namespace fs = std::filesystem;
using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::NotNull;
using ::testing::Return;

namespace {

void SetEnv(const std::string& key, const std::optional<std::string>& value) {
#ifdef _WIN32
    if (value) {
        _putenv_s(key.c_str(), value->c_str());
    } else {
        _putenv_s(key.c_str(), "");
    }
#else
    if (value) {
        setenv(key.c_str(), value->c_str(), 1);
    } else {
        unsetenv(key.c_str());
    }
#endif
}

class EnvVarGuard {
public:
    EnvVarGuard(std::string key, std::optional<std::string> value)
        : key_(std::move(key)) {
        const char* existing = std::getenv(key_.c_str());
        if (existing) {
            old_value_ = std::string(existing);
        }
        SetEnv(key_, value);
    }

    ~EnvVarGuard() { SetEnv(key_, old_value_); }

private:
    std::string key_;
    std::optional<std::string> old_value_;
};

fs::path MakeTempPath(const std::string& prefix) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (prefix + std::to_string(stamp));
}

class MockAgentClient : public dc::worker::AgentClient {
public:
    MockAgentClient() : AgentClient("http://localhost", 1000) {}

    MOCK_METHOD(bool,
                RegisterAgent,
                (const dc::worker::AgentRegistration& reg,
                 dc::worker::HeartbeatResponse* response,
                 std::string* error),
                (const, override));
    MOCK_METHOD(bool,
                SendHeartbeat,
                (const std::string& agent_id, const std::string& status, std::string* error),
                (const, override));
    MOCK_METHOD(bool,
                PollTasks,
                (const std::string& agent_id,
                 int free_slots,
                 std::vector<dc::worker::TaskDispatch>* tasks,
                 std::string* error),
                (const, override));
    MOCK_METHOD(bool,
                GetTaskState,
                (const std::string& task_id, std::string* state, std::string* error),
                (const, override));
    MOCK_METHOD(bool,
                UpdateTaskStatus,
                (const std::string& task_id,
                 const std::string& state,
                 const std::optional<int>& exit_code,
                 const std::optional<std::string>& started_at,
                 const std::optional<std::string>& finished_at,
                 const std::optional<std::string>& error_message,
                 std::string* error),
                (const, override));
    MOCK_METHOD(bool,
                UploadTaskLog,
                (const std::string& task_id,
                 const std::string& stream,
                 const std::string& data,
                 std::string* error),
                (const, override));
};

dc::worker::WorkerConfig MakeBaseConfig(const fs::path& log_dir) {
    dc::worker::WorkerConfig cfg;
    cfg.master_url = "http://localhost:12345";
    cfg.agent_id = "agent-123";
    cfg.os = "linux";
    cfg.version = "dev";
    cfg.cpu_cores = 2;
    cfg.ram_mb = 256;
    cfg.slots = 1;
    cfg.log_dir = log_dir.string();
    cfg.cancel_check_interval_sec = 1;
    cfg.upload_logs = true;
    cfg.max_upload_bytes = 1024 * 1024;
    return cfg;
}

dc::worker::TaskDispatch MakeTask(const std::string& id,
                                  const std::string& command,
                                  std::vector<std::string> args = {},
                                  std::optional<int> timeout = std::nullopt) {
    dc::worker::TaskDispatch task;
    task.task_id = id;
    task.command = command;
    task.args = std::move(args);
    task.timeout_sec = timeout;
    task.env = nlohmann::json::object();
    task.constraints = nlohmann::json::object();
    return task;
}

}  // namespace

TEST(WorkerConfigTest, LoadUsesDefaults) {
    std::vector<EnvVarGuard> guards;
    for (const char* key :
         {"MASTER_URL", "AGENT_ID", "AGENT_OS", "AGENT_VERSION", "CPU_CORES", "RAM_MB",
          "SLOTS", "WORKER_HTTP_TIMEOUT_MS", "WORKER_LOG_DIR", "CANCEL_CHECK_SEC"}) {
        guards.emplace_back(key, std::nullopt);
    }

    auto cfg = dc::worker::LoadWorkerConfigFromEnv();
    EXPECT_EQ(cfg.master_url, "http://localhost:8080");
    EXPECT_EQ(cfg.agent_id, "");
    EXPECT_EQ(cfg.version, "dev");
    EXPECT_EQ(cfg.slots, 1);
    EXPECT_EQ(cfg.http_timeout_ms, 5000);
    EXPECT_EQ(cfg.log_dir, "logs/worker");
    EXPECT_EQ(cfg.cancel_check_interval_sec, 1);
    EXPECT_GT(cfg.cpu_cores, 0);
    EXPECT_FALSE(cfg.os.empty());
}

TEST(WorkerConfigTest, LoadRespectsEnvironment) {
    EnvVarGuard master("MASTER_URL", "http://master:9000");
    EnvVarGuard agent("AGENT_ID", "agent-42");
    EnvVarGuard os("AGENT_OS", "darwin");
    EnvVarGuard version("AGENT_VERSION", "1.2.3");
    EnvVarGuard cpu("CPU_CORES", "8");
    EnvVarGuard ram("RAM_MB", "4096");
    EnvVarGuard slots("SLOTS", "3");
    EnvVarGuard timeout("WORKER_HTTP_TIMEOUT_MS", "2500");
    EnvVarGuard log_dir("WORKER_LOG_DIR", "/tmp/worker_logs");
    EnvVarGuard cancel("CANCEL_CHECK_SEC", "5");

    auto cfg = dc::worker::LoadWorkerConfigFromEnv();
    EXPECT_EQ(cfg.master_url, "http://master:9000");
    EXPECT_EQ(cfg.agent_id, "agent-42");
    EXPECT_EQ(cfg.os, "darwin");
    EXPECT_EQ(cfg.version, "1.2.3");
    EXPECT_EQ(cfg.cpu_cores, 8);
    EXPECT_EQ(cfg.ram_mb, 4096);
    EXPECT_EQ(cfg.slots, 3);
    EXPECT_EQ(cfg.http_timeout_ms, 2500);
    EXPECT_EQ(cfg.log_dir, "/tmp/worker_logs");
    EXPECT_EQ(cfg.cancel_check_interval_sec, 5);
}

TEST(WorkerConfigTest, ValidateConfig) {
    dc::worker::WorkerConfig cfg;
    std::string error;

    EXPECT_FALSE(dc::worker::ValidateWorkerConfig(cfg, &error));
    EXPECT_EQ(error, "MASTER_URL is required");

    cfg.master_url = "http://localhost";
    EXPECT_FALSE(dc::worker::ValidateWorkerConfig(cfg, &error));
    EXPECT_EQ(error, "AGENT_ID is required");

    cfg.agent_id = "agent";
    cfg.cpu_cores = 0;
    EXPECT_FALSE(dc::worker::ValidateWorkerConfig(cfg, &error));
    EXPECT_EQ(error, "CPU_CORES must be positive");

    cfg.cpu_cores = 2;
    cfg.slots = 0;
    EXPECT_FALSE(dc::worker::ValidateWorkerConfig(cfg, &error));
    EXPECT_EQ(error, "SLOTS must be positive");

    cfg.slots = 1;
    cfg.cancel_check_interval_sec = 0;
    EXPECT_FALSE(dc::worker::ValidateWorkerConfig(cfg, &error));
    EXPECT_EQ(error, "CANCEL_CHECK_SEC must be positive");

    cfg.cancel_check_interval_sec = 1;
    EXPECT_TRUE(dc::worker::ValidateWorkerConfig(cfg, &error));
}

TEST(WorkerAppConfigTest, CreateWorkerAppFailsOnInvalidConfig) {
    EnvVarGuard master("MASTER_URL", "http://localhost");
    EnvVarGuard agent("AGENT_ID", std::nullopt);
    std::string error;
    auto app = dc::worker::CreateWorkerAppFromEnv(&error);
    EXPECT_EQ(app, nullptr);
    EXPECT_THAT(error, ::testing::HasSubstr("AGENT_ID is required"));
}

TEST(WorkerAppConfigTest, CreateWorkerAppCreatesLogDir) {
    auto log_dir = MakeTempPath("dc_worker_logs_");
    EnvVarGuard master("MASTER_URL", "http://localhost");
    EnvVarGuard agent("AGENT_ID", "agent-1");
    EnvVarGuard log("WORKER_LOG_DIR", log_dir.string());
    std::string error;

    auto app = dc::worker::CreateWorkerAppFromEnv(&error);
    EXPECT_NE(app, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(fs::exists(log_dir));

    fs::remove_all(log_dir);
}

TEST(WorkerAppTest, RunOnceNoTasks) {
    auto log_dir = MakeTempPath("dc_worker_logs_");
    fs::create_directories(log_dir);
    auto cfg = MakeBaseConfig(log_dir);

    auto mock_client = std::make_unique<::testing::StrictMock<MockAgentClient>>();
    auto* client_ptr = mock_client.get();

    dc::worker::HeartbeatResponse hb_resp;
    hb_resp.heartbeat_interval_sec = 2;
    EXPECT_CALL(*client_ptr, RegisterAgent(_, NotNull(), _))
        .WillOnce(DoAll(::testing::SetArgPointee<1>(hb_resp), Return(true)));
    EXPECT_CALL(*client_ptr, SendHeartbeat(cfg.agent_id, "idle", _)).WillOnce(Return(true));
    EXPECT_CALL(*client_ptr, PollTasks(cfg.agent_id, cfg.slots, NotNull(), _))
        .WillOnce(DoAll(Invoke([](const std::string&, int, std::vector<dc::worker::TaskDispatch>* tasks,
                                  std::string*) { tasks->clear(); }),
                        Return(true)));

    dc::worker::WorkerApp app(cfg, std::move(mock_client));
    int rc = app.Run(true);
    EXPECT_EQ(rc, 0);

    fs::remove_all(log_dir);
}

#ifndef _WIN32
TEST(WorkerAppTest, UploadsLogsAfterTaskCompletion) {
    auto log_dir = MakeTempPath("dc_worker_logs_");
    fs::create_directories(log_dir);
    auto cfg = MakeBaseConfig(log_dir);

    auto mock_client = std::make_unique<::testing::StrictMock<MockAgentClient>>();
    auto* client_ptr = mock_client.get();

    dc::worker::HeartbeatResponse hb_resp;
    hb_resp.heartbeat_interval_sec = 2;
    EXPECT_CALL(*client_ptr, RegisterAgent(_, NotNull(), _))
        .WillOnce(DoAll(::testing::SetArgPointee<1>(hb_resp), Return(true)));
    EXPECT_CALL(*client_ptr, SendHeartbeat(cfg.agent_id, "idle", _)).WillOnce(Return(true));

    EXPECT_CALL(*client_ptr, PollTasks(cfg.agent_id, cfg.slots, NotNull(), _))
        .WillOnce(DoAll(Invoke([](const std::string&, int, std::vector<dc::worker::TaskDispatch>* tasks,
                                  std::string*) {
                            tasks->push_back(MakeTask("task-1",
                                                      "/bin/sh",
                                                      {"-c", "echo out; echo err 1>&2"}));
                        }),
                        Return(true)));

    EXPECT_CALL(*client_ptr, GetTaskState("task-1", NotNull(), _))
        .WillRepeatedly(DoAll(::testing::SetArgPointee<1>("running"), Return(true)));

    EXPECT_CALL(*client_ptr,
                UploadTaskLog("task-1",
                              "stdout",
                              ::testing::HasSubstr("out"),
                              _))
        .WillOnce(Return(true));
    EXPECT_CALL(*client_ptr,
                UploadTaskLog("task-1",
                              "stderr",
                              ::testing::HasSubstr("err"),
                              _))
        .WillOnce(Return(true));

    EXPECT_CALL(*client_ptr,
                UpdateTaskStatus("task-1",
                                 "running",
                                 ::testing::Truly([](const std::optional<int>& code) {
                                     return !code.has_value();
                                 }),
                                 ::testing::Truly([](const std::optional<std::string>& ts) {
                                     return ts.has_value() && !ts->empty();
                                 }),
                                 Eq(std::nullopt),
                                 Eq(std::nullopt),
                                 _))
        .WillOnce(Return(true));

    EXPECT_CALL(*client_ptr,
                UpdateTaskStatus("task-1",
                                 "succeeded",
                                 ::testing::Truly([](const std::optional<int>& code) {
                                     return code && *code == 0;
                                 }),
                                 ::testing::Truly([](const std::optional<std::string>& ts) {
                                     return ts.has_value() && !ts->empty();
                                 }),
                                 ::testing::Truly([](const std::optional<std::string>& ts) {
                                     return ts.has_value() && !ts->empty();
                                 }),
                                 ::testing::Truly([](const std::optional<std::string>& msg) {
                                     return !msg.has_value();
                                 }),
                                 _))
        .WillOnce(Return(true));

    dc::worker::WorkerApp app(cfg, std::move(mock_client));
    int rc = app.Run(true);
    EXPECT_EQ(rc, 0);

    fs::remove_all(log_dir);
}
#endif

TEST(WorkerAppTest, HandlesOsConstraintMismatch) {
    auto log_dir = MakeTempPath("dc_worker_logs_");
    fs::create_directories(log_dir);
    auto cfg = MakeBaseConfig(log_dir);

    auto mock_client = std::make_unique<::testing::StrictMock<MockAgentClient>>();
    auto* client_ptr = mock_client.get();

    dc::worker::HeartbeatResponse hb_resp;
    hb_resp.heartbeat_interval_sec = 2;
    EXPECT_CALL(*client_ptr, RegisterAgent(_, NotNull(), _))
        .WillOnce(DoAll(::testing::SetArgPointee<1>(hb_resp), Return(true)));
    EXPECT_CALL(*client_ptr, SendHeartbeat(cfg.agent_id, "idle", _)).WillOnce(Return(true));

    EXPECT_CALL(*client_ptr, PollTasks(cfg.agent_id, cfg.slots, NotNull(), _))
        .WillOnce(DoAll(Invoke([](const std::string&, int, std::vector<dc::worker::TaskDispatch>* tasks,
                                  std::string*) {
                            dc::worker::TaskDispatch t =
                                MakeTask("task-1", "/bin/echo", {"hello"});
                            t.constraints = {{"os", "windows"}};
                            tasks->push_back(std::move(t));
                        }),
                        Return(true)));

    EXPECT_CALL(*client_ptr,
                UpdateTaskStatus("task-1",
                                 "failed",
                                 ::testing::Truly([](const std::optional<int>& code) {
                                     return code && *code == 1;
                                 }),
                                 Eq(std::nullopt),
                                 ::testing::Truly([](const std::optional<std::string>& ts) {
                                     return ts.has_value() && !ts->empty();
                                 }),
                                 ::testing::Truly([](const std::optional<std::string>& msg) {
                                     return msg && *msg == "OS constraint mismatch";
                                 }),
                                 _))
        .WillOnce(Return(true));

    dc::worker::WorkerApp app(cfg, std::move(mock_client));
    int rc = app.Run(true);
    EXPECT_EQ(rc, 0);

    fs::remove_all(log_dir);
}

#ifndef _WIN32
TEST(TaskExecutorTest, RunsCommandSuccessfully) {
    auto log_root = MakeTempPath("dc_worker_exec_logs_");
    fs::create_directories(log_root);
    dc::worker::TaskExecutor executor(log_root.string());

    auto task = MakeTask("task-ok", "/bin/sh", {"-c", "echo hello"});
    auto result = executor.Run(task, [] { return false; });

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.failed_to_start);
    EXPECT_FALSE(result.timed_out);
    EXPECT_FALSE(result.canceled);
    EXPECT_TRUE(fs::exists(result.stdout_path));

    std::ifstream out(result.stdout_path);
    std::string contents((std::istreambuf_iterator<char>(out)), std::istreambuf_iterator<char>());
    EXPECT_THAT(contents, ::testing::HasSubstr("hello"));

    fs::remove_all(log_root);
}

TEST(TaskExecutorTest, TimesOutLongCommand) {
    auto log_root = MakeTempPath("dc_worker_exec_logs_");
    fs::create_directories(log_root);
    dc::worker::TaskExecutor executor(log_root.string());

    auto task = MakeTask("task-timeout", "/bin/sh", {"-c", "sleep 2"}, 1);
    auto result = executor.Run(task, [] { return false; });

    EXPECT_TRUE(result.timed_out);
    EXPECT_NE(result.exit_code, 0);
    fs::remove_all(log_root);
}

TEST(TaskExecutorTest, CancelsRunningCommand) {
    auto log_root = MakeTempPath("dc_worker_exec_logs_");
    fs::create_directories(log_root);
    dc::worker::TaskExecutor executor(log_root.string());

    auto task = MakeTask("task-cancel", "/bin/sh", {"-c", "sleep 5"});
    std::atomic<int> calls{0};
    auto result = executor.Run(task, [&] {
        return calls.fetch_add(1) > 2;
    });

    EXPECT_TRUE(result.canceled);
    EXPECT_NE(result.exit_code, 0);
    fs::remove_all(log_root);
}
#endif
