#include "api_client.hpp"

#include <chrono>

#include <httplib.h>

namespace dc {
namespace cli {

ApiClient::ApiClient(std::string base_url, int timeout_ms)
    : client_(std::make_unique<httplib::Client>(base_url)) {
    if (timeout_ms > 0) {
        auto timeout = std::chrono::milliseconds(timeout_ms);
        client_->set_connection_timeout(timeout);
        client_->set_read_timeout(timeout);
        client_->set_write_timeout(timeout);
    }
}

httplib::Headers ApiClient::DefaultHeaders() const {
    httplib::Headers headers;
    headers.emplace("Accept", "application/json");
    return headers;
}

HttpResult ApiClient::Get(const std::string& path, const httplib::Params& params) const {
    HttpResult result;
    auto headers = DefaultHeaders();
    auto res = client_->Get(path, params, headers);
    if (!res) {
        result.error = "Request failed: " + httplib::to_string(res.error());
        return result;
    }

    result.status = res->status;
    result.body = res->body;
    result.headers = res->headers;
    return result;
}

HttpResult ApiClient::Post(const std::string& path,
                           const std::string& body,
                           const std::string& content_type) const {
    HttpResult result;
    auto headers = DefaultHeaders();
    auto res = client_->Post(path, headers, body, content_type);
    if (!res) {
        result.error = "Request failed: " + httplib::to_string(res.error());
        return result;
    }

    result.status = res->status;
    result.body = res->body;
    result.headers = res->headers;
    return result;
}

}  // namespace cli
}  // namespace dc
