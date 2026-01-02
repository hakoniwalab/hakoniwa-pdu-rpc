#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/pdu_rpc_client_endpoint.hpp" // 実装で必要
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

namespace hakoniwa::pdu::rpc {

RpcServicesClient::RpcServicesClient(const std::string& node_id, const std::string& config_path, const std::string& impl_type)
    : node_id_(node_id), config_path_(config_path), impl_type_(impl_type) {
}

RpcServicesClient::~RpcServicesClient() {
    stop_all_services();
}

bool RpcServicesClient::initialize_services() {
    std::cerr << "RpcServicesClient::initialize_services() is not implemented yet." << std::endl;
    return false;
}

void RpcServicesClient::start_all_services() {
    std::cerr << "RpcServicesClient::start_all_services() is not implemented yet." << std::endl;
}

void RpcServicesClient::stop_all_services() {
    // Stop all PDU endpoints in reverse order of start
    for (auto it = pdu_endpoints_.rbegin(); it != pdu_endpoints_.rend(); ++it) {
        if (it->second) {
            it->second->stop();
        }
    }
    pdu_endpoints_.clear();
    rpc_endpoints_.clear();
    std::cout << "INFO: All RPC client services stopped." << std::endl;
}

std::future<PduData> RpcServicesClient::call_async(
    const std::string& service_name, 
    const std::string& client_name, 
    const PduData& request_pdu, 
    uint64_t timeout_usec) {
        
    auto it = rpc_endpoints_.find({service_name, client_name});
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service client not found for service: " << service_name << ", client: " << client_name << std::endl;
        std::promise<PduData> promise;
        promise.set_exception(std::make_exception_ptr(std::runtime_error("Service client not found")));
        return promise.get_future();
    }
    // `PduRpcClientEndpoint` には `call_async` のようなメソッドがあると仮定
    // return it->second->call_async(request_pdu, timeout_usec); 
    
    // 仮実装
    std::cerr << "RpcServicesClient::call_async() is not fully implemented yet." << std::endl;
    std::promise<PduData> promise;
    promise.set_exception(std::make_exception_ptr(std::runtime_error("Not implemented")));
    return promise.get_future();
}

std::optional<RpcClientEvent> RpcServicesClient::poll() {
    std::cerr << "RpcServicesClient::poll() is not implemented yet." << std::endl;
    return std::nullopt;
}

} // namespace hakoniwa::pdu::rpc
