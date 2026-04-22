#pragma once

#include <memory>
#include <string>

#include <httplib.h>

namespace dc {
namespace cli {

struct HttpResult {
    int status = 0;
    std::string body;
    httplib::Headers headers;
    std::string error;
};

class ApiClientInterface {
public:
    virtual ~ApiClientInterface() = default;

    virtual HttpResult Get(const std::string& path,
                           const httplib::Params& params = {}) const = 0;
    virtual HttpResult Post(const std::string& path,
                            const std::string& body,
                            const std::string& content_type) const = 0;
};

class ApiClient : public ApiClientInterface {
public:
    ApiClient(std::string base_url, int timeout_ms);

    HttpResult Get(const std::string& path,
                   const httplib::Params& params = {}) const override;
    HttpResult Post(const std::string& path,
                    const std::string& body,
                    const std::string& content_type) const override;

private:
    httplib::Headers DefaultHeaders() const;

    mutable std::unique_ptr<httplib::Client> client_;
};

}  // namespace cli
}  // namespace dc
