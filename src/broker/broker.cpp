#include "broker.hpp"

#include <cstdint>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <format>
#include <thread>
#include <vector>

namespace dc {
namespace broker { 

AuthentificationMethod getAuthMethod(const std::string& method) {
    if(method == "password") {
        return AuthentificationMethod::PASSWORD;
    }
    if(method == "ssl") {
        return AuthentificationMethod::SSL;
    }
    throw std::runtime_error(std::format("Unknown authentification method: {}", method));
}

const BrokerType Broker::GetBrokerType() const {
    return this->broker_type_;
}

const std::string Broker::GetConnectionString() const {
    return this->connectionString_;
}

const DbConfig Broker::GetDbConfig() const{
    return this->config_;
}

}  // namespace broker
}  // namespace dc
