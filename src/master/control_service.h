#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "log_store.h"
#include "storage.h"

namespace httplib {
class Server;
}

namespace dc {
namespace master {

struct MasterConfig {
    std::string host;
    int port = 0;
    int heartbeat_interval_sec = 30;
    int offline_after_sec = 120;
    std::string log_dir;
};

// REST API control service for Master.
class ControlService {
public:
    ControlService(MasterConfig config, Storage storage, LogStore log_store);
    ~ControlService();

    // Blocking call; returns when server is stopped.
    int Run();

private:
    void RegisterRoutes();
    void StartMaintenanceThread();
    void StopMaintenanceThread();

    MasterConfig config_;
    Storage storage_;
    LogStore log_store_;

    std::atomic<bool> running_{false};
    std::thread maintenance_thread_;
    std::unique_ptr<httplib::Server> server_;
};

}  // namespace master
}  // namespace dc
