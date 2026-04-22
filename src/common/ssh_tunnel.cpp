#include "ssh_tunnel.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winsock.h>
#else
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
extern char** environ;
#endif

#include <spdlog/spdlog.h>

namespace dc {
namespace common {
namespace {

int BindEphemeral(int preferred, std::string* error) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (error) *error = "socket() failed";
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(preferred > 0 ? preferred : 0);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "bind() failed";
#if defined(_WIN32)
        closesocket(sock);
#else
        close(sock);
#endif
        return 0;
    }

#if defined(_WIN32)
    int len;
#else
    socklen_t len;
#endif
    len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (error) *error = "getsockname() failed";
#if defined(_WIN32)
        closesocket(sock);
#else
        close(sock);
#endif
        return 0;
    }

    int port = ntohs(addr.sin_port);
#if defined(_WIN32)
    closesocket(sock);
#else
    close(sock);
#endif
    return port;
}

std::string BoolFlag(const char* name, bool enabled) {
    return std::string("-o ") + name + (enabled ? "=yes" : "=no");
}

}  // namespace

#if defined(_WIN32)
struct ProcessHandle {
    HANDLE process = nullptr;
};
#else
struct ProcessHandle {
    pid_t pid = -1;
};
#endif

struct SshTunnel::Impl {
    ProcessHandle handle;
    bool running = false;
    std::string askpass_path;
};

SshTunnel::SshTunnel(SshTunnelConfig config) : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

SshTunnel::~SshTunnel() { Stop(); }

int FindFreePort(int preferred, std::string* error) { return BindEphemeral(preferred, error); }

static std::string BuildPortForwardArg(int local_port, const SshTunnelConfig& cfg) {
    std::ostringstream oss;
    oss << local_port << ':' << cfg.remote_host << ':' << cfg.remote_port;
    return oss.str();
}

bool SshTunnel::Start(std::string* error) {
    if (impl_->running) {
        return true;
    }

    if (config_.remote_host.empty() || config_.remote_port == 0 || config_.proxy_host.empty()) {
        if (error) *error = "SSH proxy config incomplete";
        return false;
    }

    local_port_ = config_.local_port.value_or(0);
    local_port_ = FindFreePort(local_port_, error);
    if (local_port_ <= 0) {
        if (error && error->empty()) *error = "Failed to allocate local port";
        return false;
    }

    std::vector<std::string> args;
    args.push_back(config_.ssh_path.empty() ? "sshpp" : config_.ssh_path);
    args.push_back("-N");

    // Keep-alive
    args.push_back("-o");
    args.push_back("ServerAliveInterval=" + std::to_string(config_.keep_alive_interval_sec));
    args.push_back("-o");
    args.push_back("ServerAliveCountMax=" + std::to_string(config_.keep_alive_count));
    args.push_back("-o");
    args.push_back("ExitOnForwardFailure=yes");

    // Host key policy
    args.push_back(BoolFlag("StrictHostKeyChecking", config_.strict_host_key));
    if (!config_.strict_host_key) {
        args.push_back("-o");
        args.push_back("UserKnownHostsFile=/dev/null");
    }

    // Auth selection
    if (!config_.proxy_key_path.empty()) {
        args.push_back("-i");
        args.push_back(config_.proxy_key_path);
    }
    if (!config_.proxy_password.empty()) {
        args.push_back("-o");
        args.push_back("BatchMode=no");
        args.push_back("-o");
        args.push_back("PreferredAuthentications=password");
        args.push_back("-o");
        args.push_back("PubkeyAuthentication=no");
        args.push_back("-o");
        args.push_back("NumberOfPasswordPrompts=1");
    }

    args.push_back("-L");
    args.push_back(BuildPortForwardArg(local_port_, config_));

    // target: user@proxy_host -p port
    std::string target = config_.proxy_host;
    if (!config_.proxy_user.empty()) {
        target = config_.proxy_user + "@" + target;
    }
    args.push_back(target);
    args.push_back("-p");
    args.push_back(std::to_string(config_.proxy_port));

    // Build environment (for password via askpass)
    std::vector<std::pair<std::string, std::string>> env;
    if (!config_.proxy_password.empty()) {
        env.emplace_back("DISPLAY", "1");
        if (impl_->askpass_path.empty()) {
            if (!config_.askpass_path.empty()) {
                impl_->askpass_path = config_.askpass_path;
            }
#if defined(_WIN32) && defined(DC_SSH_ASKPASS_PATH)
            if (impl_->askpass_path.empty()) {
                impl_->askpass_path = DC_SSH_ASKPASS_PATH;
            }
#endif
        }
        env.emplace_back("SSH_ASKPASS", impl_->askpass_path.empty() ? "ssh-askpass" : impl_->askpass_path);
        env.emplace_back("SSH_PROXY_PASSWORD", config_.proxy_password);
    }

#if defined(_WIN32)
    // Prepare command line
    std::ostringstream cmdline;
    for (const auto& a : args) {
        // naive quoting for simple cases
        bool has_space = a.find(' ') != std::string::npos;
        if (has_space) cmdline << '"';
        cmdline << a;
        if (has_space) cmdline << '"';
        cmdline << ' ';
    }
    std::string cmd = cmdline.str();
    if (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Build environment block
    std::wstring env_block;
    if (!env.empty()) {
        for (auto& kv : env) {
            std::wstring wkey(kv.first.begin(), kv.first.end());
            std::wstring wval(kv.second.begin(), kv.second.end());
            env_block.append(wkey).push_back(L'=');
            env_block.append(wval).push_back(L'\0');
        }
        env_block.push_back(L'\0');
    }

    std::wstring wcmd(cmd.begin(), cmd.end());
    BOOL ok = CreateProcessW(nullptr,
                             wcmd.data(),
                             nullptr,
                             nullptr,
                             FALSE,
                             CREATE_NO_WINDOW,
                             env_block.empty() ? nullptr : env_block.data(),
                             nullptr,
                             &si,
                             &pi);
    if (!ok) {
        if (error) *error = "CreateProcessW failed";
        return false;
    }
    impl_->handle.process = pi.hProcess;
    CloseHandle(pi.hThread);
    impl_->running = true;
    return true;
#else
    // POSIX spawn
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    // Build envp
    std::vector<std::string> extra_env_strs;
    std::vector<char*> envp;
    if (!env.empty()) {
        for (auto& kv : env) {
            extra_env_strs.push_back(kv.first + "=" + kv.second);
        }
    }
    for (char** e = environ; e && *e; ++e) {
        envp.push_back(*e);
    }
    for (auto& s : extra_env_strs) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), envp.data());
    if (rc != 0) {
        if (error) *error = "posix_spawnp failed";
        return false;
    }
    impl_->handle.pid = pid;
    impl_->running = true;
    return true;
#endif
}

bool SshTunnel::IsAlive() const {
    if (!impl_->running) return false;
#if defined(_WIN32)
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->handle.process, &code)) {
        return code == STILL_ACTIVE;
    }
    return false;
#else
    int status = 0;
    pid_t res = waitpid(impl_->handle.pid, &status, WNOHANG);
    return res == 0;
#endif
}

void SshTunnel::Stop() {
    if (!impl_->running) return;
#if defined(_WIN32)
    TerminateProcess(impl_->handle.process, 1);
    WaitForSingleObject(impl_->handle.process, 2000);
    CloseHandle(impl_->handle.process);
#else
    kill(impl_->handle.pid, SIGTERM);
    waitpid(impl_->handle.pid, nullptr, 0);
#endif
    impl_->running = false;
}

}  // namespace common
}  // namespace dc
