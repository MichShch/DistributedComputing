#include "master/api_mappers.h"
#include "master/log_store.h"
#include "master/status.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

nlohmann::json MakeAgentPayload() {
    return nlohmann::json{
        {"os", "linux"},
        {"version", "1.0"},
        {"resources",
         {{"cpu_cores", 4},
          {"ram_mb", 2048},
          {"slots", 2}}},
    };
}

fs::path MakeTempDir() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("dc_log_store_test_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void WriteFile(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

}  // namespace

TEST(StatusTest, AgentStatusFromApiParsesValidValues) {
    EXPECT_EQ(dc::master::AgentStatusFromApi("idle"), dc::master::AgentStatus::Idle);
    EXPECT_EQ(dc::master::AgentStatusFromApi("BUSY"), dc::master::AgentStatus::Busy);
    EXPECT_EQ(dc::master::AgentStatusFromApi("Offline"), dc::master::AgentStatus::Offline);
}

TEST(StatusTest, AgentStatusFromApiRejectsInvalid) {
    EXPECT_FALSE(dc::master::AgentStatusFromApi(""));
    EXPECT_FALSE(dc::master::AgentStatusFromApi("unknown"));
}

TEST(StatusTest, TaskStateFromApiParsesValidValues) {
    EXPECT_EQ(dc::master::TaskStateFromApi("queued"), dc::master::TaskState::Queued);
    EXPECT_EQ(dc::master::TaskStateFromApi("Running"), dc::master::TaskState::Running);
    EXPECT_EQ(dc::master::TaskStateFromApi("SUCCEEDED"), dc::master::TaskState::Succeeded);
    EXPECT_EQ(dc::master::TaskStateFromApi("failed"), dc::master::TaskState::Failed);
    EXPECT_EQ(dc::master::TaskStateFromApi("Canceled"), dc::master::TaskState::Canceled);
}

TEST(StatusTest, TaskStateFromApiRejectsInvalid) {
    EXPECT_FALSE(dc::master::TaskStateFromApi(""));
    EXPECT_FALSE(dc::master::TaskStateFromApi("done"));
}

TEST(StatusTest, StatusToDbAndApiMappings) {
    EXPECT_STREQ(dc::master::AgentStatusToDb(dc::master::AgentStatus::Idle), "Idle");
    EXPECT_STREQ(dc::master::AgentStatusToDb(dc::master::AgentStatus::Busy), "Busy");
    EXPECT_STREQ(dc::master::AgentStatusToDb(dc::master::AgentStatus::Offline), "Offline");

    EXPECT_STREQ(dc::master::AgentStatusToApi(dc::master::AgentStatus::Idle), "idle");
    EXPECT_STREQ(dc::master::AgentStatusToApi(dc::master::AgentStatus::Busy), "busy");
    EXPECT_STREQ(dc::master::AgentStatusToApi(dc::master::AgentStatus::Offline), "offline");

    EXPECT_STREQ(dc::master::TaskStateToDb(dc::master::TaskState::Queued), "Queued");
    EXPECT_STREQ(dc::master::TaskStateToDb(dc::master::TaskState::Running), "Running");
    EXPECT_STREQ(dc::master::TaskStateToDb(dc::master::TaskState::Succeeded), "Succeeded");
    EXPECT_STREQ(dc::master::TaskStateToDb(dc::master::TaskState::Failed), "Failed");
    EXPECT_STREQ(dc::master::TaskStateToDb(dc::master::TaskState::Canceled), "Canceled");

    EXPECT_STREQ(dc::master::TaskStateToApi(dc::master::TaskState::Queued), "queued");
    EXPECT_STREQ(dc::master::TaskStateToApi(dc::master::TaskState::Running), "running");
    EXPECT_STREQ(dc::master::TaskStateToApi(dc::master::TaskState::Succeeded), "succeeded");
    EXPECT_STREQ(dc::master::TaskStateToApi(dc::master::TaskState::Failed), "failed");
    EXPECT_STREQ(dc::master::TaskStateToApi(dc::master::TaskState::Canceled), "canceled");
}

TEST(ApiMappersTest, ParseAgentInputRequiresFields) {
    dc::master::AgentInput out;
    std::string error;
    EXPECT_FALSE(dc::master::api::ParseAgentInput("agent-1", nlohmann::json::object(), &out, &error));
    EXPECT_EQ(error, "Missing required fields");
}

TEST(ApiMappersTest, ParseAgentInputRejectsInvalidResources) {
    auto payload = MakeAgentPayload();
    payload["resources"]["cpu_cores"] = "bad";
    dc::master::AgentInput out;
    std::string error;
    EXPECT_FALSE(dc::master::api::ParseAgentInput("agent-1", payload, &out, &error));
    EXPECT_EQ(error, "Invalid resources payload");
}

TEST(ApiMappersTest, ParseAgentInputAcceptsValidPayload) {
    auto payload = MakeAgentPayload();
    dc::master::AgentInput out;
    std::string error;
    EXPECT_TRUE(dc::master::api::ParseAgentInput("agent-1", payload, &out, &error));
    EXPECT_EQ(out.agent_id, "agent-1");
    EXPECT_EQ(out.os, "linux");
    EXPECT_EQ(out.version, "1.0");
    EXPECT_EQ(out.cpu_cores, 4);
    EXPECT_EQ(out.ram_mb, 2048);
    EXPECT_EQ(out.slots, 2);
}

TEST(ApiMappersTest, ParseAgentHeartbeatValidatesStatus) {
    dc::master::AgentHeartbeat out;
    std::string error;
    EXPECT_FALSE(dc::master::api::ParseAgentHeartbeat("agent-1",
                                                     nlohmann::json{{"status", "unknown"}},
                                                     &out,
                                                     &error));
    EXPECT_EQ(error, "Invalid agent status");
}

TEST(ApiMappersTest, ParseAgentHeartbeatAcceptsValidStatus) {
    dc::master::AgentHeartbeat out;
    std::string error;
    EXPECT_TRUE(dc::master::api::ParseAgentHeartbeat("agent-1",
                                                    nlohmann::json{{"status", "busy"}},
                                                    &out,
                                                    &error));
    EXPECT_EQ(out.agent_id, "agent-1");
    EXPECT_EQ(out.status, dc::master::AgentStatus::Busy);
}

TEST(ApiMappersTest, ParseTaskCreateRequiresFields) {
    dc::master::TaskInput out;
    std::string error;
    EXPECT_FALSE(dc::master::api::ParseTaskCreate(nlohmann::json::object(), &out, &error));
    EXPECT_EQ(error, "Missing command");
}

TEST(ApiMappersTest, ParseTaskCreateRequiresCommandString) {
    dc::master::TaskInput out;
    std::string error;
    nlohmann::json payload{{"command", 123}};
    EXPECT_FALSE(dc::master::api::ParseTaskCreate(payload, &out, &error));
    EXPECT_EQ(error, "Missing command");
}

TEST(ApiMappersTest, ParseTaskCreateAppliesDefaults) {
    dc::master::TaskInput out;
    std::string error;
    nlohmann::json payload{{"command", "run"}};
    EXPECT_TRUE(dc::master::api::ParseTaskCreate(payload, &out, &error));
    EXPECT_TRUE(out.args.is_array());
    EXPECT_TRUE(out.env.is_object());
    EXPECT_FALSE(out.timeout_sec.has_value());
    EXPECT_TRUE(out.constraints.is_object());
}

TEST(ApiMappersTest, ParseTaskCreateReadsOptionalFields) {
    dc::master::TaskInput out;
    std::string error;
    nlohmann::json payload{
        {"command", "run"},
        {"args", nlohmann::json::array({"one", "two"})},
        {"env", nlohmann::json{{"KEY", "VALUE"}}},
        {"timeout_sec", 30},
        {"constraints", nlohmann::json{{"os", "linux"}}},
    };
    EXPECT_TRUE(dc::master::api::ParseTaskCreate(payload, &out, &error));
    EXPECT_EQ(out.args.size(), 2);
    EXPECT_EQ(out.args[0], "one");
    EXPECT_EQ(out.env["KEY"], "VALUE");
    EXPECT_EQ(out.timeout_sec.value(), 30);
    EXPECT_EQ(out.constraints["os"], "linux");
}

TEST(ApiMappersTest, ParseTaskCreateFiltersNonStringArgs) {
    dc::master::TaskInput out;
    std::string error;
    nlohmann::json payload{
        {"command", "run"},
        {"args", nlohmann::json::array({"keep", 2, true, "also-keep"})},
    };
    EXPECT_TRUE(dc::master::api::ParseTaskCreate(payload, &out, &error));
    EXPECT_EQ(out.args, nlohmann::json::array({"keep", "also-keep"}));
}

TEST(ApiMappersTest, ParseTaskStatusUpdateValidatesState) {
    dc::master::api::TaskStatusUpdate out;
    std::string error;
    EXPECT_FALSE(dc::master::api::ParseTaskStatusUpdate(nlohmann::json::object(),
                                                       &out,
                                                       &error));
    EXPECT_EQ(error, "Missing state");
}

TEST(ApiMappersTest, ParseTaskStatusUpdateReadsOptionalFields) {
    dc::master::api::TaskStatusUpdate out;
    std::string error;
    nlohmann::json payload{
        {"state", "failed"},
        {"exit_code", 127},
        {"started_at", "2024-01-01T00:00:00Z"},
        {"finished_at", "2024-01-01T00:01:00Z"},
        {"error_message", "boom"},
    };
    EXPECT_TRUE(dc::master::api::ParseTaskStatusUpdate(payload, &out, &error));
    EXPECT_EQ(out.state, dc::master::TaskState::Failed);
    EXPECT_EQ(out.exit_code.value(), 127);
    EXPECT_EQ(out.started_at.value(), "2024-01-01T00:00:00Z");
    EXPECT_EQ(out.finished_at.value(), "2024-01-01T00:01:00Z");
    EXPECT_EQ(out.error_message.value(), "boom");
}

TEST(ApiMappersTest, ParseTaskStatusUpdateTreatsEmptyOptionalFieldsAsNullopt) {
    dc::master::api::TaskStatusUpdate out;
    std::string error;
    nlohmann::json payload{
        {"state", "failed"},
        {"started_at", ""},
        {"finished_at", ""},
        {"error_message", ""},
    };
    EXPECT_TRUE(dc::master::api::ParseTaskStatusUpdate(payload, &out, &error));
    EXPECT_FALSE(out.started_at.has_value());
    EXPECT_FALSE(out.finished_at.has_value());
    EXPECT_FALSE(out.error_message.has_value());
}

TEST(ApiMappersTest, ParseTaskIdAcceptsOnlyPositiveIntegers) {
    EXPECT_TRUE(dc::master::api::ParseTaskId("1").has_value());
    EXPECT_TRUE(dc::master::api::ParseTaskId("12345").has_value());
    EXPECT_FALSE(dc::master::api::ParseTaskId("").has_value());
    EXPECT_FALSE(dc::master::api::ParseTaskId("0").has_value());
    EXPECT_FALSE(dc::master::api::ParseTaskId("-1").has_value());
    EXPECT_FALSE(dc::master::api::ParseTaskId("bad.id").has_value());
}

TEST(ApiMappersTest, IsValidTaskStateTransitionMatrix) {
    using dc::master::TaskState;
    EXPECT_TRUE(dc::master::api::IsValidTaskStateTransition(TaskState::Queued, TaskState::Running));
    EXPECT_TRUE(dc::master::api::IsValidTaskStateTransition(TaskState::Queued, TaskState::Canceled));
    EXPECT_FALSE(dc::master::api::IsValidTaskStateTransition(TaskState::Queued, TaskState::Failed));

    EXPECT_TRUE(dc::master::api::IsValidTaskStateTransition(TaskState::Running, TaskState::Succeeded));
    EXPECT_TRUE(dc::master::api::IsValidTaskStateTransition(TaskState::Running, TaskState::Failed));
    EXPECT_TRUE(dc::master::api::IsValidTaskStateTransition(TaskState::Running, TaskState::Canceled));
    EXPECT_FALSE(dc::master::api::IsValidTaskStateTransition(TaskState::Running, TaskState::Queued));

    EXPECT_FALSE(dc::master::api::IsValidTaskStateTransition(TaskState::Succeeded, TaskState::Running));
    EXPECT_FALSE(dc::master::api::IsValidTaskStateTransition(TaskState::Failed, TaskState::Canceled));
}

TEST(ApiMappersTest, TaskRecordToJsonIncludesOptionals) {
    dc::master::TaskRecord record;
    record.task_id = 1;
    record.state = dc::master::TaskState::Running;
    record.command = "run";
    record.args = nlohmann::json::array({1});
    record.env = nlohmann::json::object();
    record.timeout_sec = 10;
    record.assigned_agent = "agent-1";
    record.created_at = "2024-01-01T00:00:00Z";
    record.started_at = "2024-01-01T00:00:01Z";
    record.finished_at = std::nullopt;
    record.exit_code = std::nullopt;
    record.error_message = "err";
    record.constraints = nlohmann::json::object();

    auto json = dc::master::api::TaskRecordToJson(record);
    EXPECT_EQ(json["task_id"], 1);
    EXPECT_EQ(json["state"], "running");
    EXPECT_EQ(json["timeout_sec"], 10);
    EXPECT_EQ(json["assigned_agent"], "agent-1");
    EXPECT_EQ(json["started_at"], "2024-01-01T00:00:01Z");
    EXPECT_EQ(json["error_message"], "err");
    EXPECT_TRUE(json.contains("constraints"));
}

TEST(ApiMappersTest, TaskDispatchToJsonSkipsNullConstraints) {
    dc::master::TaskDispatch dispatch;
    dispatch.task_id = 1;
    dispatch.command = "run";
    dispatch.args = nlohmann::json::array();
    dispatch.env = nlohmann::json::object();
    dispatch.constraints = nullptr;
    auto json = dc::master::api::TaskDispatchToJson(dispatch);
    EXPECT_FALSE(json.contains("constraints"));
}

TEST(LogStoreTest, ReadAllReturnsMissingWhenNoLog) {
    fs::path root = MakeTempDir();
    dc::master::LogStore store(root.string());

    auto result = store.ReadAll("task-1", "stdout");
    EXPECT_FALSE(result.exists);
    EXPECT_EQ(result.size_bytes, 0u);

    fs::remove_all(root);
}

TEST(LogStoreTest, ReadAllReadsFullContent) {
    fs::path root = MakeTempDir();
    WriteFile(root / "task-1" / "stdout.log", "hello");

    dc::master::LogStore store(root.string());
    auto result = store.ReadAll("task-1", "stdout");
    EXPECT_TRUE(result.exists);
    EXPECT_EQ(result.size_bytes, 5u);
    EXPECT_EQ(result.data, "hello");

    fs::remove_all(root);
}

TEST(LogStoreTest, ReadFromOffsetReturnsTail) {
    fs::path root = MakeTempDir();
    WriteFile(root / "task-1" / "stdout.log", "abcdef");

    dc::master::LogStore store(root.string());
    auto result = store.ReadFromOffset("task-1", "stdout", 2);
    EXPECT_TRUE(result.exists);
    EXPECT_EQ(result.size_bytes, 6u);
    EXPECT_EQ(result.data, "cdef");

    auto beyond = store.ReadFromOffset("task-1", "stdout", 9);
    EXPECT_TRUE(beyond.exists);
    EXPECT_EQ(beyond.data, "");

    fs::remove_all(root);
}

TEST(LogStoreTest, RejectsPathTraversal) {
    fs::path root = MakeTempDir();
    dc::master::LogStore store(root.string());

    auto result = store.ReadAll("..", "stdout");
    EXPECT_FALSE(result.exists);
    EXPECT_EQ(result.size_bytes, 0u);

    fs::remove_all(root);
}

TEST(LogStoreTest, RefreshesMetadataOnRead) {
    fs::path root = MakeTempDir();
    WriteFile(root / "task-1" / "stdout.log", "data");

    dc::master::LogStore store(root.string());
    auto result = store.ReadAll("task-1", "stdout");
    EXPECT_TRUE(result.exists);

    fs::path meta_path = root / "task-1" / "meta.json";
    EXPECT_TRUE(fs::exists(meta_path));

    fs::remove_all(root);
}

TEST(LogStoreTest, WriteAllStoresContentAndRefreshesMeta) {
    fs::path root = MakeTempDir();
    dc::master::LogStore store(root.string());

    EXPECT_TRUE(store.WriteAll("task-1", "stdout", "hello"));

    auto result = store.ReadAll("task-1", "stdout");
    EXPECT_TRUE(result.exists);
    EXPECT_EQ(result.data, "hello");

    fs::path meta_path = root / "task-1" / "meta.json";
    EXPECT_TRUE(fs::exists(meta_path));

    fs::remove_all(root);
}
