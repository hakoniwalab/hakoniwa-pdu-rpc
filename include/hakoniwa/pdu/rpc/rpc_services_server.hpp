#pragma once

#include "pdu_rpc_server_endpoint.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/time_source_factory.hpp"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <map>
#include <thread>

namespace hakoniwa::pdu::rpc {

class RpcServicesServer {
public:
    RpcServicesServer(const std::string& node_id, const std::string& impl_type, const std::string& service_config_path, uint64_t delta_time_usec, std::string time_source_type = "real")
        : node_id_(node_id), impl_type_(impl_type), service_config_path_(service_config_path), delta_time_usec_(delta_time_usec) 
        {
            time_source_ = hakoniwa::time_source::create_time_source(time_source_type, delta_time_usec);
        }
    virtual ~RpcServicesServer(); // Removed = default;
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
    void create_reply_buffer(const HakoCpp_ServiceRequestHeader& header, Hako_uint8 status, Hako_int32 result_code, PduData& pdu) {
        auto it = rpc_endpoints_.find(header.service_name);
        if (it != rpc_endpoints_.end()) {
            it->second->create_reply_buffer(header, status, result_code, pdu);
        } else {
            std::cerr << "ERROR: Service '" << header.service_name << "' not found for creating reply buffer." << std::endl;
        }
    }

    ServerEventType poll(RpcRequest& request);

    void send_reply(HakoCpp_ServiceRequestHeader header, const PduData& pdu)
    {
        auto it = rpc_endpoints_.find(header.service_name);
        if (it != rpc_endpoints_.end()) {
            it->second->send_reply(header.client_name, pdu);
        } else {
            std::cerr << "ERROR: Service not found for sending reply: " << header.service_name << std::endl;
        }
    }
    void send_cancel_reply(HakoCpp_ServiceRequestHeader header, const PduData& pdu)
    {
        auto it = rpc_endpoints_.find(header.service_name);
        if (it != rpc_endpoints_.end()) {
            it->second->send_cancel_reply(header.client_name, pdu);
        } else {
            std::cerr << "ERROR: Service not found for sending cancel reply: " << header.service_name << std::endl;
        }
    }

private:
    //(nodeId, endpointId), endpoint
    std::map<std::pair<std::string, std::string>, std::shared_ptr<hakoniwa::pdu::Endpoint>> pdu_endpoints_;
    //service_name, endpoint
    std::map<std::string, std::shared_ptr<IPduRpcServerEndpoint>> rpc_endpoints_;
    std::string node_id_;
    std::string impl_type_;
    std::string service_config_path_;
    uint64_t delta_time_usec_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
};

} // namespace hakoniwa::pdu::rpc
