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
    RpcServicesServer(const std::string& impl_type) : impl_type_(impl_type) {}
    virtual ~RpcServicesServer() = default;
    bool initialize_services();

private:
    //service_name, endpoint
    std::map<std::string, std::shared_ptr<IPduRpcServerEndpoint>> endpoints_;
    std::string impl_type_;
};

} // namespace hakoniwa::pdu::rpc
