#pragma once

#include "pdu_rpc_server_endpoint.hpp"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <map>

namespace hakoniwa::pdu::rpc {

class RpcServicesServer {
public:
    RpcServicesServer(const std::string& node_id, const std::string& impl_type, const std::string& service_config_path, uint64_t delta_time_usec) 
        : node_id_(node_id), impl_type_(impl_type), service_config_path_(service_config_path), delta_time_usec_(delta_time_usec) {}
    virtual ~RpcServicesServer() = default;
    bool initialize_services();

private:
    //service_name, endpoint
    std::map<std::string, std::shared_ptr<IPduRpcServerEndpoint>> endpoints_;
    std::string node_id_;
    std::string impl_type_;
    std::string service_config_path_;
    uint64_t delta_time_usec_;
};

} // namespace hakoniwa::pdu::rpc
