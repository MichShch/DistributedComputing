#include "task_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "common/time_utils.hpp"

namespace dc {
namespace worker {

namespace {

#if defined(_WIN32)

std::string QuoteArg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string quoted = "\"";
    for (char c : arg) {
        if (c == '"') {
            quoted.push_back('\\');
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

#else

std::vector<char*> BuildArgv(const TaskDispatch& task, std::vector<std::string>* broker) {
    broker->clear();
    broker->reserve(task.args.size() + 1);
    broker->push_back(task.command);
    for (const auto& arg : task.args) {
        broker->push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(broker->size() + 1);
    for (auto& s : *broker) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    return argv;
}

#endif

}  // namespace

TaskExecutor::TaskExecutor(std::string log_root) : log_root_(std::move(log_root)) {}

std::string TaskExecutor::EnsureTaskLogDir(const std::string& task_id) {
    std::filesystem::path dir = std::filesystem::path(log_root_) / task_id;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        spdlog::warn("Failed to create log dir {}: {}", dir.string(), ec.message());
    }
    return dir.string();
}

TaskExecutionResult TaskExecutor::Run(const TaskDispatch& task,
                                    const std::function<bool()>& is_canceled,
                                    const std::function<void()>& doHeartbeat) {
    TaskExecutionResult result;
    result.started_at = dc::common::NowUtcIso8601();

    const int timeout_sec = task.timeout_sec.value_or(0);
    const bool has_timeout = timeout_sec > 0;

    const std::string log_dir = EnsureTaskLogDir(task.task_id);
    result.stdout_path = (std::filesystem::path(log_dir) / "stdout.log").string();
    result.stderr_path = (std::filesystem::path(log_dir) / "stderr.log").string();

#if defined(_WIN32)
    std::string cmdline = QuoteArg(task.command);
    for (const auto& arg : task.args) {
        cmdline.push_back(' ');
        cmdline.append(QuoteArg(arg));
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> prev_env;
    if (task.env.is_object()) {
        for (auto it = task.env.begin(); it != task.env.end(); ++it) {
            if (!it.value().is_string()) {
                continue;
            }
            std::optional<std::string> previous;
            if (const char* existing = std::getenv(it.key().c_str())) {
                previous = existing;
            }
            if (_putenv_s(it.key().c_str(), it.value().get<std::string>().c_str()) != 0) {
                spdlog::warn("Failed to set environment variable {}", it.key());
            }
            prev_env.emplace_back(it.key(), std::move(previous));
        }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = CreateFileA(result.stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = CreateFileA(result.stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        result.failed_to_start = true;
        result.error_message = "Failed to open log files";
        return result;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    CloseHandle(hErr);

    if (!ok) {
        result.failed_to_start = true;
        result.error_message = "CreateProcess failed";
        return result;
    }

    DWORD remaining_ms = has_timeout ? static_cast<DWORD>(timeout_sec * 1000) : INFINITE;
    const DWORD slice_ms = 500;
    while (true) {
        DWORD wait_ms = has_timeout ? std::min(slice_ms, remaining_ms) : slice_ms;
        DWORD wait_code = WaitForSingleObject(pi.hProcess, wait_ms);
        if (wait_code == WAIT_OBJECT_0) {
            break;
        }
        if (wait_code == WAIT_FAILED) {
            result.failed_to_start = true;
            result.error_message = "WaitForSingleObject failed";
            break;
        }
        if (is_canceled && is_canceled()) {
            TerminateProcess(pi.hProcess, 1);
            result.canceled = true;
            break;
        }
        if (has_timeout) {
            if (remaining_ms <= wait_ms) {
                TerminateProcess(pi.hProcess, 1);
                result.timed_out = true;
                break;
            }
            remaining_ms -= wait_ms;
        }
        doHeartbeat();
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    for (const auto& entry : prev_env) {
        if (entry.second.has_value()) {
            _putenv_s(entry.first.c_str(), entry.second->c_str());
        } else {
            _putenv_s(entry.first.c_str(), "");
        }
    }

#else
    // POSIX path
    std::vector<std::string> arg_broker;
    std::vector<char*> argv = BuildArgv(task, &arg_broker);

    pid_t pid = fork();
    if (pid == 0) {
        int fd_out = ::open(result.stdout_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int fd_err = ::open(result.stderr_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd_out >= 0) {
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }
        if (fd_err >= 0) {
            dup2(fd_err, STDERR_FILENO);
            close(fd_err);
        }

        if (task.env.is_object()) {
            for (auto it = task.env.begin(); it != task.env.end(); ++it) {
                if (it.value().is_string()) {
                    setenv(it.key().c_str(), it.value().get<std::string>().c_str(), 1);
                }
            }
        }

        execvp(task.command.c_str(), argv.data());
        _exit(127);
    }
    if (pid < 0) {
        result.failed_to_start = true;
        result.error_message = "fork failed";
        return result;
    }

    auto start = std::chrono::steady_clock::now();
    int status = 0;
    while (true) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {  // child exited
            break;
        }
        if (done == -1) {
            if (errno == EINTR) {
                continue;
            }
            result.failed_to_start = true;
            result.error_message = "waitpid failed";
            return result;
        }
        bool canceled = is_canceled && is_canceled();
        if (canceled) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.canceled = true;
            break;
        }
        if (has_timeout) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= std::chrono::seconds(timeout_sec)) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                result.timed_out = true;
                break;
            }
        }
        doHeartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
#endif

    result.finished_at = dc::common::NowUtcIso8601();
    return result;
}

}  // namespace worker
}  // namespace dc
