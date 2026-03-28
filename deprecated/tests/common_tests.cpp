#include "common/env.h"
#include "common/logging.h"
#include "common/string_utils.h"
#include "common/time_utils.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace {

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

bool IsIso8601Utc(const std::string& value) {
    if (value.size() != 20) {
        return false;
    }
    const int digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int pos : digit_positions) {
        if (!std::isdigit(static_cast<unsigned char>(value[pos]))) {
            return false;
        }
    }
    return value[4] == '-' && value[7] == '-' && value[10] == 'T' &&
           value[13] == ':' && value[16] == ':' && value[19] == 'Z';
}

}  // namespace

TEST(EnvTest, GetEnvOrDefaultMissingReturnsDefault) {
    const char* key = "DC_TEST_ENV_MISSING";
    UnsetEnvVar(key);
    EXPECT_EQ(dc::common::GetEnvOrDefault(key, "fallback"), "fallback");
}

TEST(EnvTest, GetEnvOrDefaultReturnsValueWhenSet) {
    const char* key = "DC_TEST_ENV_VALUE";
    SetEnvVar(key, "value");
    EXPECT_EQ(dc::common::GetEnvOrDefault(key, "fallback"), "value");
    UnsetEnvVar(key);
}

TEST(EnvTest, GetEnvOrDefaultNullKeyReturnsDefault) {
    EXPECT_EQ(dc::common::GetEnvOrDefault(nullptr, "fallback"), "fallback");
}

TEST(EnvTest, GetEnvIntOrDefaultMissingReturnsDefault) {
    const char* key = "DC_TEST_ENV_INT_MISSING";
    UnsetEnvVar(key);
    EXPECT_EQ(dc::common::GetEnvIntOrDefault(key, 7), 7);
}

TEST(EnvTest, GetEnvIntOrDefaultParsesInteger) {
    const char* key = "DC_TEST_ENV_INT_VALUE";
    SetEnvVar(key, "42");
    EXPECT_EQ(dc::common::GetEnvIntOrDefault(key, 7), 42);
    UnsetEnvVar(key);
}

TEST(EnvTest, GetEnvIntOrDefaultInvalidReturnsDefault) {
    const char* key = "DC_TEST_ENV_INT_INVALID";
    SetEnvVar(key, "12x");
    EXPECT_EQ(dc::common::GetEnvIntOrDefault(key, 7), 7);
    UnsetEnvVar(key);
}

TEST(EnvTest, GetEnvIntOrDefaultEmptyParsesZero) {
    const char* key = "DC_TEST_ENV_INT_EMPTY";
    SetEnvVar(key, "");
    EXPECT_EQ(dc::common::GetEnvIntOrDefault(key, 7), 0);
    UnsetEnvVar(key);
}

TEST(StringUtilsTest, ToLowerCopyConvertsAscii) {
    EXPECT_EQ(dc::common::ToLowerCopy("HeLLo123"), "hello123");
}

TEST(StringUtilsTest, ToLowerCopyLeavesLowercaseUntouched) {
    EXPECT_EQ(dc::common::ToLowerCopy("already_lower"), "already_lower");
}

TEST(StringUtilsTest, IsValidUtf8AcceptsAsciiAndUtf8) {
    EXPECT_TRUE(dc::common::IsValidUtf8("plain ascii"));
    EXPECT_TRUE(dc::common::IsValidUtf8("hello \xD0\xBC\xD0\xB8\xD1\x80"));
}

TEST(StringUtilsTest, SanitizeUtf8LossyReplacesInvalidBytes) {
    std::string bad = "abc";
    bad.push_back(static_cast<char>(0x8A));
    bad += "z";

    std::string sanitized = dc::common::SanitizeUtf8Lossy(bad);
    EXPECT_TRUE(dc::common::IsValidUtf8(sanitized));
    EXPECT_EQ(sanitized, "abc\xEF\xBF\xBDz");
}

TEST(TimeUtilsTest, NowUtcIso8601HasExpectedFormat) {
    std::string now = dc::common::NowUtcIso8601();
    EXPECT_TRUE(IsIso8601Utc(now));
}

TEST(LoggingTest, ParseLogLevelRecognizesAliases) {
    auto fallback = spdlog::level::warn;
    EXPECT_EQ(dc::common::ParseLogLevel("trace", fallback), spdlog::level::trace);
    EXPECT_EQ(dc::common::ParseLogLevel("DEBUG", fallback), spdlog::level::debug);
    EXPECT_EQ(dc::common::ParseLogLevel("info", fallback), spdlog::level::info);
    EXPECT_EQ(dc::common::ParseLogLevel("warning", fallback), spdlog::level::warn);
    EXPECT_EQ(dc::common::ParseLogLevel("err", fallback), spdlog::level::err);
    EXPECT_EQ(dc::common::ParseLogLevel("crit", fallback), spdlog::level::critical);
    EXPECT_EQ(dc::common::ParseLogLevel("off", fallback), spdlog::level::off);
}

TEST(LoggingTest, ParseLogLevelFallsBackOnInvalid) {
    auto fallback = spdlog::level::info;
    EXPECT_EQ(dc::common::ParseLogLevel("nope", fallback), fallback);
    EXPECT_EQ(dc::common::ParseLogLevel("", fallback), fallback);
}
