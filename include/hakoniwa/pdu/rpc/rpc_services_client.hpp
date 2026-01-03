#pragma once

#include "pdu_rpc_types.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "pdu_rpc_client_endpoint.hpp" // For IPduRpcClientEndpoint
#include "pdu_rpc_client_endpoint_impl.hpp" // For PduRpcClientEndpointImpl
#include "pdu_rpc_time.hpp" // For ITimeSource
#include <string>
#include <memory>
#include <map>
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace hakoniwa::pdu::rpc {

class RpcServicesClient {
public:
    RpcServicesClient(const std::string& node_id, const std::string& client_name, const std::string& config_path, const std::string& impl_type = "PduRpcClientEndpointImpl", uint64_t delta_time_usec = 1000);
    ~RpcServicesClient();

    bool initialize_services();
    void start_all_services();
    void stop_all_services();
    bool is_pdu_end_point_running() {
        for (auto& pdu_endpoint_pair : pdu_endpoints_) {
            auto& pdu_endpoint = pdu_endpoint_pair.second;
            bool running = false;
            pdu_endpoint->is_running(running);
            if (!running) {
                return false;
            }
        }
        return true;
    }

    bool call(const std::string& service_name, const PduData& request_pdu, uint64_t timeout_usec);
    ClientEventType poll(std::string& service_name_out, RpcResponse& response_out);
    bool send_cancel_request(const std::string& service_name);
    void create_request_buffer(const std::string& service_name, PduData& pdu);
    void create_request_buffer(const std::string& service_name, Hako_uint8 opcode, PduData& pdu);

private:
    std::string node_id_;
    std::string client_name_; // Single client identity
    std::string config_path_;
    std::string impl_type_;
    uint64_t delta_time_usec_; // For client endpoint initialization

    // Low-level PDU endpoints (node_id, endpoint_id) -> endpoint
    std::map<std::pair<std::string, std::string>, std::shared_ptr<hakoniwa::pdu::Endpoint>> pdu_endpoints_;
    // RPC client endpoints (service_name) -> endpoint
    std::map<std::string, std::shared_ptr<IPduRpcClientEndpoint>> rpc_endpoints_;
    std::shared_ptr<ITimeSource> time_source_;
};

} // namespace hakoniwa::pdu::rpc