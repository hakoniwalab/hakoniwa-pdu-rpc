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
#include <filesystem>

#include "endpoints_loader.hpp"

namespace hakoniwa::pdu::rpc {


// Constructor is already defined in the header with its initializer list.
// If debug prints are needed in the constructor, the definition must be moved from header to here.
// For now, we rely on existing initializers.

// Destructor is explicitly defaulted in header.
// If custom logic (like debug prints) is needed, it must be defined here, NOT defaulted.
RpcServicesServer::~RpcServicesServer() {
    stop_all_services();
}

bool RpcServicesServer::initialize_services() {
    std::cout << "INFO: Initializing RPC Services Server for node: " << this->node_id_ << std::endl;
    std::cout << "INFO: service_config_path: " << this->service_config_path_ << std::endl;
    fs::path file_path(this->service_config_path_);
    fs::path parent_abs = fs::absolute(file_path.parent_path());
    std::cout << "INFO: service_config_path parent: " << parent_abs << std::endl;
    std::ifstream ifs(service_config_path_);
    if (!ifs.is_open()) {
        std::cerr << "ERROR: Failed to open service config file: " << service_config_path_ << std::endl;
        return false;
    }
    std::cout << "INFO: Successfully opened service config file." << std::endl;
    nlohmann::json json_config;
    try {
        ifs >> json_config;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse service config JSON: " << e.what() << std::endl;
        std::cout.flush();
        return false;
    }

    try {
        int pdu_meta_data_size = json_config.value("pduMetaDataSize", 8);
        if (!load_and_initialize_pdu_endpoints(this->node_id_, json_config, parent_abs, this->pdu_endpoints_)) {
            stop_all_services();
            return false;
        }

        // Then, initialize services that are meant for this server
        for (const auto& service_entry : json_config["services"]) {
            std::string service_name = service_entry["name"];
            std::cout << "DEBUG: Looking for server endpoint for service: " << service_name << std::endl;
            bool found = false;
            std::string server_endpoint_id;
            if (!service_entry.contains("server_endpoints") || !service_entry["server_endpoints"].is_array()) {
                std::cerr << "ERROR: 'server_endpoints' section missing or not an array for service " << service_name << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }
            for (const auto& server_ep : service_entry["server_endpoints"]) {
                std::cout << "DEBUG: Checking server endpoint: " << server_ep.dump() << std::endl;
                if (server_ep["nodeId"] != this->node_id_) {
                    continue;
                }
                server_endpoint_id = server_ep["endpointId"];
                found = true;
                break;
            }
            if (!found) {
                std::cerr << "ERROR: PDU Endpoint not found for service " << service_name 
                          << " on node " << this->node_id_ << " with endpoint " << server_endpoint_id << ". Check 'endpoints' section in config." << std::endl;
                std::cout.flush();
                stop_all_services();
                return false; // This is a configuration error
            }
            
            auto pdu_ep_key = std::make_pair(this->node_id_, server_endpoint_id);
            auto it = pdu_endpoints_.find(pdu_ep_key);
            if (it == pdu_endpoints_.end()) {
                std::cerr << "ERROR: PDU Endpoint not found for service " << service_name 
                          << " on node " << this->node_id_ << " with endpoint " << server_endpoint_id << ". Check 'endpoints' section in config." << std::endl;
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
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
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
        endpoint_pair.second->clear_pending_requests();
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
        endpoint->clear_pending_requests();
        auto impl = std::dynamic_pointer_cast<RpcServerEndpointImpl>(endpoint);
        if (impl) {
            impl->clear_all_instances();
        }
    }
}
} // namespace hakoniwa::pdu::rpc
