#pragma once

#include "rpc_server_endpoint.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/time_source/time_source_factory.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hakoniwa::pdu::rpc {

class RpcServicesServer {
public:
    RpcServicesServer(const std::string& node_id, const std::string& impl_type, const std::string& service_config_path, uint64_t delta_time_usec, std::string time_source_type = "real")
        : node_id_(node_id), impl_type_(impl_type), service_config_path_(service_config_path), delta_time_usec_(delta_time_usec)
        {
            time_source_ = hakoniwa::time_source::create_time_source(time_source_type, delta_time_usec);
        }
    virtual ~RpcServicesServer();

    bool initialize_services(
        std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container,
        std::optional<std::string> client_node_id = std::nullopt);

    // Initialize all configured services on one already-opened Endpoint.
    // This path is used by server-side communication multiplexers, where each
    // accepted transport session becomes its own Endpoint instance.
    bool initialize_services(
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
        std::optional<std::string> client_node_id = std::nullopt);

    bool start_all_services();
    void stop_all_services();
    void clear_all_instances();
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
    bool initialize_services_impl(
        std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_override,
        std::optional<std::string> client_node_id);

    //service_name, endpoint
    std::map<std::string, std::shared_ptr<IRpcServerEndpoint>> rpc_endpoints_;
    std::string node_id_;
    std::string impl_type_;
    std::string service_config_path_;
    uint64_t delta_time_usec_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container_;
};

} // namespace hakoniwa::pdu::rpc
