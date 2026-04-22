#pragma once

#include <memory>
#include <optional>
#include <string>

namespace dc {
namespace common {

struct SshTunnelConfig {
    std::string ssh_path = "ssh";
    std::string proxy_host;
    int proxy_port = 22;
    std::string proxy_user;
    std::string proxy_key_path;
    std::string proxy_password;  // optional; used via askpass
    std::string askpass_path;    // optional; auto-filled on Windows when empty
    std::string remote_host;
    int remote_port = 0;
    std::optional<int> local_port;  // if empty, auto-assign
    bool strict_host_key = true;
    int keep_alive_interval_sec = 30;
    int keep_alive_count = 3;
};

class SshTunnel {
public:
    explicit SshTunnel(SshTunnelConfig config);
    ~SshTunnel();

    bool Start(std::string* error);
    bool IsAlive() const;
    void Stop();
    int local_port() const { return local_port_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SshTunnelConfig config_;
    int local_port_ = 0;
};

int FindFreePort(int preferred, std::string* error);

}  // namespace common
}  // namespace dc
