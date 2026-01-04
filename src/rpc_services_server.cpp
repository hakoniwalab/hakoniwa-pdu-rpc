#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_server_endpoint_impl.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp" // For HAKO_PDU_ENDPOINT_DIRECTION_INOUT
#include "hakoniwa/pdu/endpoint.hpp" // For hakoniwa::pdu::Endpoint
#include "hakoniwa/time_source/real_time_source.hpp" // For RealTimeSource
#include "hakoniwa/time_source/virtual_time_source.hpp" // For VirtualTimeSource
#include "hakoniwa/time_source/hakoniwa_time_source.hpp" // For HakoniwaTimeSource
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>
#include <map>
#include <memory>

namespace hakoniwa::pdu::rpc {

// Helper function to find config_path for a given nodeId and endpointId
static std::string find_endpoint_config_path(const nlohmann::json& json_config, const std::string& node_id, const std::string& endpoint_id) {
    for (const auto& node_entry : json_config["endpoints"]) {
        if (node_entry["nodeId"] == node_id) {
            for (const auto& ep_entry : node_entry["endpoints"]) {
                if (ep_entry["id"] == endpoint_id) {
                    return ep_entry["config_path"];
                }
            }
        }
    }
    return ""; // Not found
}

// Constructor is already defined in the header with its initializer list.
// If debug prints are needed in the constructor, the definition must be moved from header to here.
// For now, we rely on existing initializers.

// Destructor is explicitly defaulted in header.
// If custom logic (like debug prints) is needed, it must be defined here, NOT defaulted.
RpcServicesServer::~RpcServicesServer() {
    stop_all_services();
}

bool RpcServicesServer::initialize_services() {
    std::ifstream ifs(service_config_path_);
    if (!ifs.is_open()) {
        return false;
    }
    nlohmann::json json_config;
    try {
        ifs >> json_config;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse service config JSON: " << e.what() << std::endl;
        std::cout.flush();
        return false;
    }

    // A single time source can be shared among all services managed by this server

    try {
        if (!json_config.contains("endpoints")) {
            std::cerr << "ERROR: Service config missing 'endpoints' section." << std::endl;
            stop_all_services();
            return false;
        }
        int pdu_meta_data_size = json_config.value("pduMetaDataSize", 8);
        // First, process all endpoints relevant to this server node
        for (const auto& node_entry : json_config["endpoints"]) {
            std::string current_node_id = node_entry["nodeId"];
            if (current_node_id != this->node_id_) {
                continue; // Only process endpoints for this server's node
            }

            for (const auto& ep_entry : node_entry["endpoints"]) {
                std::string endpoint_id = ep_entry["id"];
                std::string config_path_for_endpoint = ep_entry["config_path"];

                std::string pdu_endpoint_name = current_node_id + "-" + endpoint_id;
                std::shared_ptr<hakoniwa::pdu::Endpoint> pdu_endpoint = 
                    std::make_shared<hakoniwa::pdu::Endpoint>(pdu_endpoint_name, HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
                
                if (pdu_endpoint->open(config_path_for_endpoint) != HAKO_PDU_ERR_OK) {
                    std::cerr << "ERROR: Failed to open PDU endpoint config: " << config_path_for_endpoint << " for node '" << current_node_id << "' endpoint '" << endpoint_id << "'" << std::endl;
                    std::cout.flush();
                    stop_all_services();
                    return false;
                }
                pdu_endpoints_[{current_node_id, endpoint_id}] = pdu_endpoint;
                std::cout << "INFO: Successfully initialized PDU endpoint '" << pdu_endpoint_name << "' with config '" << config_path_for_endpoint << "'" << std::endl;
                std::cout.flush();
            }
        }

        // Then, initialize services that are meant for this server
        for (const auto& service_entry : json_config["services"]) {
            std::string server_node_id_in_config = service_entry["server_endpoint"]["nodeId"];

            // Only initialize services that this RpcServicesServer instance is responsible for
            if (server_node_id_in_config != this->node_id_) {
                continue;
            }

            std::string service_name = service_entry["name"];
            std::string server_endpoint_id = service_entry["server_endpoint"]["endpointId"];
            
            auto pdu_ep_key = std::make_pair(server_node_id_in_config, server_endpoint_id);
            auto it = pdu_endpoints_.find(pdu_ep_key);
            if (it == pdu_endpoints_.end()) {
                std::cerr << "ERROR: PDU Endpoint not found for service " << service_name 
                          << " on node " << server_node_id_in_config << " with endpoint " << server_endpoint_id << ". Check 'endpoints' section in config." << std::endl;
                std::cout.flush();
                stop_all_services();
                return false; // This is a configuration error
            }
            std::shared_ptr<hakoniwa::pdu::Endpoint> pdu_endpoint = it->second;

            std::shared_ptr<IRpcServerEndpoint> rpc_server_endpoint;
            if (impl_type_ == "RpcServerEndpointImpl") {
                rpc_server_endpoint = std::make_shared<RpcServerEndpointImpl>(service_name, delta_time_usec_, pdu_endpoint, time_source_);
            } else {
                std::cerr << "ERROR: Unsupported RPC Server Endpoint Implementation Type: " << impl_type_ << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }
            if (!rpc_server_endpoint->initialize(service_entry, pdu_meta_data_size)) {
                std::cerr << "ERROR: Failed to initialize RPC server endpoint for service " << service_name << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }

            rpc_endpoints_[service_name] = rpc_server_endpoint;
            std::cout << "INFO: Successfully initialized service: " << service_name << " on node " << this->node_id_ << std::endl;
            std::cout.flush();
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Malformed service config JSON: " << e.what() << std::endl;
        std::cout.flush();
        stop_all_services();
        return false;
    }
    return true;
}

bool RpcServicesServer::start_all_services() {
    for (auto& pdu_endpoint_pair : pdu_endpoints_) {
        auto& pdu_endpoint = pdu_endpoint_pair.second;
        if (pdu_endpoint->start() != HAKO_PDU_ERR_OK) {
            std::cerr << "ERROR: Failed to start PDU endpoint for " << pdu_endpoint_pair.first.first << ":" << pdu_endpoint_pair.first.second << std::endl;
            std::cout.flush();
            return false;
        } else {
            std::cout << "INFO: Started PDU endpoint for " << pdu_endpoint_pair.first.first << ":" << pdu_endpoint_pair.first.second << std::endl;
            std::cout.flush();
        }
    }
    return true;
}

void RpcServicesServer::stop_all_services() {
    for (auto& pdu_endpoint_pair : pdu_endpoints_) {
        pdu_endpoint_pair.second->stop();
        pdu_endpoint_pair.second->close();
    }
    for (auto& endpoint_pair : rpc_endpoints_) {
        auto& endpoint = endpoint_pair.second;
        auto impl = std::dynamic_pointer_cast<RpcServerEndpointImpl>(endpoint);
        if (impl) {
            impl->clear_pending_requests();
        }
    }
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

void RpcServicesServer::clear_all_instances() {
    for (auto& endpoint_pair : rpc_endpoints_) {
        auto& endpoint = endpoint_pair.second;
        auto impl = std::dynamic_pointer_cast<RpcServerEndpointImpl>(endpoint);
        if (impl) {
            impl->clear_all_instances();
        }
    }
}
} // namespace hakoniwa::pdu::rpc