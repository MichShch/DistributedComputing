#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "common/string_utils.hpp"

namespace dc {
namespace broker {

enum class AgentStatus { Idle, Busy, Offline };
enum class TaskState { Queued, Running, Succeeded, Failed, Canceled };

inline std::optional<AgentStatus> AgentStatusFromApi(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string lower = dc::common::ToLowerCopy(std::string(value));
    if (lower == "idle") {
        return AgentStatus::Idle;
    }
    if (lower == "busy") {
        return AgentStatus::Busy;
    }
    if (lower == "offline") {
        return AgentStatus::Offline;
    }
    return std::nullopt;
}

inline std::optional<TaskState> TaskStateFromApi(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string lower = dc::common::ToLowerCopy(std::string(value));
    if (lower == "queued") {
        return TaskState::Queued;
    }
    if (lower == "running") {
        return TaskState::Running;
    }
    if (lower == "succeeded") {
        return TaskState::Succeeded;
    }
    if (lower == "failed") {
        return TaskState::Failed;
    }
    if (lower == "canceled") {
        return TaskState::Canceled;
    }
    return std::nullopt;
}

inline const char* AgentStatusToDb(AgentStatus status) {
    switch (status) {
        case AgentStatus::Idle:
            return "Idle";
        case AgentStatus::Busy:
            return "Busy";
        case AgentStatus::Offline:
            return "Offline";
        default:
            return "Idle";
    }
}

inline const char* TaskStateToDb(TaskState state) {
    switch (state) {
        case TaskState::Queued:
            return "Queued";
        case TaskState::Running:
            return "Running";
        case TaskState::Succeeded:
            return "Succeeded";
        case TaskState::Failed:
            return "Failed";
        case TaskState::Canceled:
            return "Canceled";
        default:
            return "Queued";
    }
}

inline const char* AgentStatusToApi(AgentStatus status) {
    switch (status) {
        case AgentStatus::Idle:
            return "idle";
        case AgentStatus::Busy:
            return "busy";
        case AgentStatus::Offline:
            return "offline";
        default:
            return "idle";
    }
}

inline const char* TaskStateToApi(TaskState state) {
    switch (state) {
        case TaskState::Queued:
            return "queued";
        case TaskState::Running:
            return "running";
        case TaskState::Succeeded:
            return "succeeded";
        case TaskState::Failed:
            return "failed";
        case TaskState::Canceled:
            return "canceled";
        default:
            return "queued";
    }
}


}  // namespace broker
}  // namespace dc
