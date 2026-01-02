#pragma once

#include "pdu_rpc_server_endpoint.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <map>
#include <thread>

namespace hakoniwa::pdu::rpc {

class RpcServicesServer {
public:
    RpcServicesServer(const std::string& node_id, const std::string& impl_type, const std::string& service_config_path, uint64_t delta_time_usec) 
        : node_id_(node_id), impl_type_(impl_type), service_config_path_(service_config_path), delta_time_usec_(delta_time_usec) {}
    virtual ~RpcServicesServer() = default;
    bool initialize_services();
    void start_all_services() {
        for (auto& pdu_endpoint_pair : pdu_endpoints_) {
            auto& pdu_endpoint = pdu_endpoint_pair.second;
            if (pdu_endpoint->start() != HAKO_PDU_ERR_OK) {
                std::cerr << "ERROR: Failed to start PDU endpoint for service " << pdu_endpoint->get_name() << std::endl;
            } else {
                std::cout << "INFO: Started PDU endpoint for service " << pdu_endpoint->get_name() << std::endl;
            }
        }
        //wait for all services to be running
        bool all_running = false;
        while (!all_running) {
            all_running = true;
            for (auto& pdu_endpoint_pair : pdu_endpoints_) {
                auto& pdu_endpoint = pdu_endpoint_pair.second;
                bool running = false;
                pdu_endpoint->is_running(running);
                if (!running) {
                    all_running = false;
                    break;
                }
            }
            if (!all_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
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
};

} // namespace hakoniwa::pdu::rpc
