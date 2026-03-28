#include "cli/main.cpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

class FakeApiClient : public dc::cli::ApiClientInterface {
public:
    struct Request {
        std::string method;
        std::string path;
        httplib::Params params;
        std::string body;
        std::string content_type;
    };

    mutable std::vector<Request> requests;
    mutable std::vector<dc::cli::HttpResult> get_results;
    mutable std::vector<dc::cli::HttpResult> post_results;
    dc::cli::HttpResult default_result;

    FakeApiClient() { default_result.status = 200; }

    dc::cli::HttpResult Get(const std::string& path,
                            const httplib::Params& params = {}) const override {
        requests.push_back(Request{"GET", path, params, "", ""});
        if (!get_results.empty()) {
            auto result = get_results.front();
            get_results.erase(get_results.begin());
            return result;
        }
        return default_result;
    }

    dc::cli::HttpResult Post(const std::string& path,
                             const std::string& body,
                             const std::string& content_type) const override {
        requests.push_back(Request{"POST", path, {}, body, content_type});
        if (!post_results.empty()) {
            auto result = post_results.front();
            post_results.erase(post_results.begin());
            return result;
        }
        return default_result;
    }
};

void SetEnvVar(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value ? value : "");
#else
    if (value) {
        setenv(key, value, 1);
    } else {
        unsetenv(key);
    }
#endif
}

void UnsetEnvVar(const char* key) {
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

}  // namespace

TEST(CliUtilsTest, StartsWithAndParseInt) {
    EXPECT_TRUE(dc::cli::StartsWith("abcdef", "abc"));
    EXPECT_FALSE(dc::cli::StartsWith("abcdef", "abd"));

    auto parsed = dc::cli::ParseInt("42");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, 42);

    auto bad = dc::cli::ParseInt("12x");
    EXPECT_FALSE(bad.has_value());
}

TEST(CliUtilsTest, ValidatesTaskIdAndScheme) {
    EXPECT_TRUE(dc::cli::IsValidTaskId("123"));
    EXPECT_FALSE(dc::cli::IsValidTaskId("task_1-OK"));
    EXPECT_FALSE(dc::cli::IsValidTaskId("bad id"));

    EXPECT_EQ(dc::cli::EnsureScheme("example.com"), "http://example.com");
    EXPECT_EQ(dc::cli::EnsureScheme("https://example.com"), "https://example.com");
}

TEST(CliUtilsTest, ParsesGlobalOptionsWithOverrides) {
    dc::cli::GlobalOptions options;
    std::vector<std::string> rest;
    std::string error;
    std::vector<std::string> args = {
        "--base-url", "example.com", "--host", "10.0.0.2", "--port", "9090",
        "--timeout-ms=4000",       "--json=true",         "--verbose=false",
        "tasks",     "list"};

    ASSERT_TRUE(dc::cli::ParseGlobalOptions(args, &options, &rest, &error));
    EXPECT_EQ(options.base_url, "example.com");
    EXPECT_EQ(options.host, "10.0.0.2");
    EXPECT_EQ(options.port, 9090);
    EXPECT_EQ(options.timeout_ms, 4000);
    EXPECT_TRUE(options.json_output);
    EXPECT_FALSE(options.verbose);
    ASSERT_EQ(rest.size(), 2u);
    EXPECT_EQ(rest[0], "tasks");
    EXPECT_EQ(rest[1], "list");
}

TEST(CliUtilsTest, ParsesGlobalOptionsDefaultsFromEnv) {
    SetEnvVar("MASTER_HOST", "192.168.1.10");
    SetEnvVar("MASTER_PORT", "12345");

    dc::cli::GlobalOptions options;
    std::vector<std::string> rest;
    std::string error;
    std::vector<std::string> args = {"tasks", "ls"};

    ASSERT_TRUE(dc::cli::ParseGlobalOptions(args, &options, &rest, &error));
    EXPECT_EQ(options.host, "192.168.1.10");
    EXPECT_EQ(options.port, 12345);

    UnsetEnvVar("MASTER_HOST");
    UnsetEnvVar("MASTER_PORT");
}

TEST(CliUtilsTest, ParseGlobalOptionsFailsOnUnknownOption) {
    dc::cli::GlobalOptions options;
    std::vector<std::string> rest;
    std::string error;
    std::vector<std::string> args = {"--bad-flag"};

    EXPECT_FALSE(dc::cli::ParseGlobalOptions(args, &options, &rest, &error));
    EXPECT_EQ(error, "Unknown option: --bad-flag");
}

TEST(CliUtilsTest, BuildBaseUrlPrefersExplicitBase) {
    dc::cli::GlobalOptions options;
    options.base_url = "example.com";
    options.host = "ignored";
    options.port = 9999;
    EXPECT_EQ(dc::cli::BuildBaseUrl(options), "http://example.com");
}

TEST(CliFormattersTest, PrintsKeyValueTable) {
    std::vector<std::pair<std::string, std::string>> rows = {{"field1", "value1"},
                                                             {"field2", "value2"}};
    testing::internal::CaptureStdout();
    dc::cli::PrintKeyValueTable(std::cout, rows);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Field"), std::string::npos);
    EXPECT_NE(output.find("field1"), std::string::npos);
    EXPECT_NE(output.find("value2"), std::string::npos);
}

TEST(CliHandlersTest, HandleTasksListRendersTableAndRecordsRequest) {
    FakeApiClient client;
    client.get_results.push_back(
        dc::cli::HttpResult{200, R"({"tasks":[{"task_id":"t1","state":"queued"}]})", {}, ""});

    dc::cli::GlobalOptions options;

    testing::internal::CaptureStdout();
    int code = dc::cli::HandleTasksList(client, options, {});
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(code, 0);
    EXPECT_NE(output.find("t1"), std::string::npos);
    ASSERT_EQ(client.requests.size(), 1u);
    EXPECT_EQ(client.requests[0].path, "/api/v1/tasks");
    auto limit_it = client.requests[0].params.find("limit");
    ASSERT_NE(limit_it, client.requests[0].params.end());
    EXPECT_EQ(limit_it->second, "50");
}

TEST(CliHandlersTest, HandleTasksSubmitBuildsPayloadAndPosts) {
    FakeApiClient client;
    client.post_results.push_back(dc::cli::HttpResult{201, R"({"task_id":42})", {}, ""});

    dc::cli::GlobalOptions options;
    std::vector<std::string> args = {
        "--cmd", "echo", "--arg", "hello", "--env", "KEY=VALUE",
        "--cpu", "2",    "--ram", "256",  "--label", "gpu"};

    testing::internal::CaptureStdout();
    int code = dc::cli::HandleTasksSubmit(client, options, args);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(code, 0);
    EXPECT_NE(output.find("42"), std::string::npos);
    ASSERT_EQ(client.requests.size(), 1u);
    EXPECT_EQ(client.requests[0].path, "/api/v1/tasks");

    auto body = nlohmann::json::parse(client.requests[0].body);
    EXPECT_FALSE(body.contains("task_id"));
    EXPECT_EQ(body["command"], "echo");
    EXPECT_EQ(body["args"][0], "hello");
    EXPECT_EQ(body["env"]["KEY"], "VALUE");
    EXPECT_EQ(body["constraints"]["cpu_cores"], 2);
    EXPECT_EQ(body["constraints"]["ram_mb"], 256);
    EXPECT_EQ(body["constraints"]["labels"][0], "gpu");
}

TEST(CliHandlersTest, HandleAgentsListRendersTable) {
    FakeApiClient client;
    client.get_results.push_back(dc::cli::HttpResult{
        200, R"({"agents":[{"agent_id":"a1","status":"idle"}]})", {}, ""});

    dc::cli::GlobalOptions options;

    testing::internal::CaptureStdout();
    int code = dc::cli::HandleAgentsList(client, options, {});
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(code, 0);
    EXPECT_NE(output.find("a1"), std::string::npos);
    ASSERT_EQ(client.requests.size(), 1u);
    EXPECT_EQ(client.requests[0].path, "/api/v1/agents");
}

TEST(CliHandlersTest, HandleErrorResultUsesErrorMessage) {
    dc::cli::HttpResult result;
    result.error = "network down";

    testing::internal::CaptureStderr();
    int code = dc::cli::HandleErrorResult(result, false);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(code, 4);
    EXPECT_NE(err.find("network down"), std::string::npos);
}
