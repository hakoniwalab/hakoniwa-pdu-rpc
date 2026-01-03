#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/pdu_rpc_server_endpoint_impl.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace hakoniwa::pdu::rpc {

bool RpcServicesServer::initialize_services() {
    std::ifstream ifs(service_config_path_);
    if (!ifs.is_open()) {
        std::cerr << "ERROR: Failed to open config file: " << service_config_path_ << std::endl;
        return false;
    }

    nlohmann::json config_json;
    try {
        ifs >> config_json;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse config file: " << e.what() << std::endl;
        return false;
    }

    // Pre-initialize all unique endpoints
    if (config_json.contains("endpoints")) {
        for (const auto& node : config_json["endpoints"]) {
            std::string nodeId = node["nodeId"];
            // Process only endpoints belonging to this server's node
            if (nodeId != this->node_id_) {
                continue;
            }
            for (const auto& ep_config : node["endpoints"]) {
                std::string endpointId = ep_config["id"];
                std::string config_path = ep_config["config_path"];
                
                auto pdu_endpoint = std::make_shared<hakoniwa::pdu::Endpoint>(nodeId, HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
                if (pdu_endpoint->open(config_path) != HAKO_PDU_ERR_OK) {
                    std::cerr << "ERROR: Failed to open PDU endpoint with config " << config_path << std::endl;
                    return false; // Early exit if any endpoint fails to open
                }
                pdu_endpoints_[{nodeId, endpointId}] = pdu_endpoint;
                std::cout << "INFO: Successfully opened endpoint " << endpointId << " on node " << nodeId << std::endl;
            }
        }
    }
    
    // A single time source can be shared among all services managed by this server
    //TODO : Use a mock time source for testing
    auto time_source = std::make_shared<RealTimeSource>();

    for (const auto& service : config_json["services"]) {
        std::string server_node_id = service["server_endpoint"]["nodeId"];

        // Process only services that are meant for this server instance
        if (server_node_id != this->node_id_) {
            continue;
        }

        std::string service_name = service["name"];
        std::string server_endpoint_id = service["server_endpoint"]["endpointId"];
        
        auto it = pdu_endpoints_.find({server_node_id, server_endpoint_id});
        if (it == pdu_endpoints_.end()) {
            std::cerr << "ERROR: Pre-initialized endpoint not found for service " << service_name 
                      << " on node " << server_node_id << " with endpoint " << server_endpoint_id << std::endl;
            continue;
        }
        auto pdu_endpoint = it->second;

        std::shared_ptr<IPduRpcServerEndpoint> endpoint;
        if (impl_type_ == "PduRpcServerEndpointImpl") {
            endpoint = std::make_shared<PduRpcServerEndpointImpl>(service_name, delta_time_usec_, pdu_endpoint, time_source);
        } else {
            std::cerr << "ERROR: Unknown implementation type: " << impl_type_ << std::endl;
            continue;
        }

        if (!endpoint->initialize(service)) {
            std::cerr << "ERROR: Failed to initialize services for " << service_name << std::endl;
            continue;
        }

        rpc_endpoints_[service_name] = endpoint;
        std::cout << "INFO: Successfully initialized service: " << service_name << std::endl;
    }

    return true;
}

ServerEventType RpcServicesServer::poll(RpcRequest& request)
{
    for (auto& endpoint_pair : rpc_endpoints_) {
        auto& endpoint = endpoint_pair.second;
        ServerEventType event = endpoint->poll(request);
        if (event != ServerEventType::NONE) {
            return event;
        }
    }
    return ServerEventType::NONE;
}
} // namespace hakoniwa::pdu::rpc