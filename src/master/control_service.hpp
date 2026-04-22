#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "log_store.hpp"
#include "broker/broker.hpp"

namespace httplib {
class Request;
class Response;
class Server;
}

namespace dc {
using namespace broker;
namespace master {

struct MasterConfig {
    std::string host;
    int port = 0;
    int heartbeat_interval_sec = 30;
    int offline_after_sec = 120;
    std::string log_dir;
    std::size_t max_log_upload_bytes = 10 * 1024 * 1024;  // 10MB
};

// REST API control service for Master.
class ControlService {
public:
    ControlService(MasterConfig config, Broker* broker, LogStore log_store);
    ~ControlService();

    // Blocking call; returns when server is stopped.
    int Run();

private:
    void RegisterRoutes();
    void HandleUpsertAgent(const httplib::Request& req, httplib::Response& res);
    void HandleAgentHeartbeat(const httplib::Request& req, httplib::Response& res);
    void HandlePollTasks(const httplib::Request& req, httplib::Response& res);
    void HandleCreateTask(const httplib::Request& req, httplib::Response& res);
    void HandleGetTask(const httplib::Request& req, httplib::Response& res);
    void HandleListTasks(const httplib::Request& req, httplib::Response& res);
    void HandleUpdateTaskStatus(const httplib::Request& req, httplib::Response& res);
    void HandleCancelTask(const httplib::Request& req, httplib::Response& res);
    void HandleGetAgent(const httplib::Request& req, httplib::Response& res);
    void HandleListAgents(const httplib::Request& req, httplib::Response& res);
    void HandleGetLogs(const httplib::Request& req, httplib::Response& res);
    void HandleTailLogs(const httplib::Request& req, httplib::Response& res);
    void HandleUploadLogs(const httplib::Request& req, httplib::Response& res);
    void HandleHealth(const httplib::Request& req, httplib::Response& res);
    void StartMaintenanceThread();
    void StopMaintenanceThread();

    MasterConfig config_;
    std::unique_ptr<Broker> broker_;
    LogStore log_store_;

    std::atomic<bool> running_{false};
    std::thread maintenance_thread_;
    std::unique_ptr<httplib::Server> server_;
};

}  // namespace master
}  // namespace dc
