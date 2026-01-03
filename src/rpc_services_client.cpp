#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

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

// Constructor: takes client_name as a specific identity for this client instance
RpcServicesClient::RpcServicesClient(const std::string& node_id, const std::string& client_name, const std::string& config_path, const std::string& impl_type, uint64_t delta_time_usec)
    : node_id_(node_id), client_name_(client_name), config_path_(config_path), impl_type_(impl_type), delta_time_usec_(delta_time_usec) {
        time_source_ = std::make_shared<RealTimeSource>();
}

// Destructor: ensures all services are stopped cleanly
RpcServicesClient::~RpcServicesClient() {
    stop_all_services();
}

bool RpcServicesClient::initialize_services() {
    std::ifstream ifs(config_path_);
    if (!ifs.is_open()) {
        std::cerr << "ERROR: Failed to open service config file: " << config_path_ << std::endl;
        return false;
    }
    nlohmann::json json_config;
    try {
        ifs >> json_config;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse service config JSON: " << e.what() << std::endl;
        return false;
    }

    try {
        for (const auto& service_entry : json_config["services"]) {
            std::string service_name = service_entry["name"];
            
            // Look for client-specific configuration for this service
            bool client_config_found = false;
            std::string client_ep_node_id;
            std::string client_ep_id;
            
            for (const auto& client_spec : service_entry["clients"]) {
                if (client_spec["name"] == this->client_name_) {
                    client_ep_node_id = client_spec["client_endpoint"]["nodeId"];
                    client_ep_id = client_spec["client_endpoint"]["endpointId"];
                    client_config_found = true;
                    break;
                }
            }

            if (!client_config_found) {
                continue; 
            }

            if (client_ep_node_id != this->node_id_) {
                continue;
            }

            // Find config_path for the low-level PDU Endpoint
            std::string pdu_endpoint_config_path = find_endpoint_config_path(json_config, client_ep_node_id, client_ep_id);
            if (pdu_endpoint_config_path.empty()) {
                std::cerr << "ERROR: PDU Endpoint config_path not found for node '" << client_ep_node_id << "' and endpoint '" << client_ep_id << "'" << std::endl;
                std::cout.flush();
                return false;
            }

            // Create low-level PDU endpoint
            std::string pdu_endpoint_name = client_ep_node_id + "-" + client_ep_id;
            std::shared_ptr<hakoniwa::pdu::Endpoint> pdu_endpoint = 
                std::make_shared<hakoniwa::pdu::Endpoint>(pdu_endpoint_name, HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
            
            if (pdu_endpoint->open(pdu_endpoint_config_path) != HAKO_PDU_ERR_OK) {
                std::cerr << "ERROR: Failed to open PDU endpoint config: " << pdu_endpoint_config_path << " for service " << service_name << std::endl;
                std::cout.flush();
                return false;
            }
            pdu_endpoints_[{client_ep_node_id, client_ep_id}] = pdu_endpoint; // Keyed by (nodeId, endpointId)

            // Create high-level RPC client endpoint
            std::shared_ptr<IPduRpcClientEndpoint> rpc_client_endpoint = 
                std::make_shared<PduRpcClientEndpointImpl>(service_name, client_name_, delta_time_usec_, pdu_endpoint, time_source_);
            
            if (!rpc_client_endpoint->initialize(service_entry)) {
                std::cerr << "ERROR: Failed to initialize RPC client endpoint for service " << service_name << std::endl;
                std::cout.flush();
                return false;
            }
            rpc_endpoints_[service_name] = rpc_client_endpoint; // Keyed by service_name
            std::cout << "INFO: Successfully initialized client for service: " << service_name << " on node " << this->node_id_ << std::endl;
            std::cout.flush();
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Malformed service config JSON: " << e.what() << std::endl;
        std::cout.flush();
        return false;
    }
    return true;
}

void RpcServicesClient::start_all_services() {
    for (auto& pdu_endpoint_pair : pdu_endpoints_) {
        auto& pdu_endpoint = pdu_endpoint_pair.second;
        if (pdu_endpoint->start() != HAKO_PDU_ERR_OK) {
            std::cerr << "ERROR: Failed to start PDU endpoint for " << pdu_endpoint_pair.first.first << ":" << pdu_endpoint_pair.first.second << std::endl;
            std::cout.flush();
        } else {
            std::cout << "INFO: Started PDU endpoint for " << pdu_endpoint_pair.first.first << ":" << pdu_endpoint_pair.first.second << std::endl;
            std::cout.flush();
        }
    }
}

void RpcServicesClient::stop_all_services() {
    for (auto& pdu_endpoint_pair : pdu_endpoints_) {
        pdu_endpoint_pair.second->stop();
        pdu_endpoint_pair.second->close();
    }
}

bool RpcServicesClient::call(const std::string& service_name, const PduData& request_pdu, uint64_t timeout_usec) {
    auto it = rpc_endpoints_.find(service_name);
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service '" << service_name << "' not found for RPC call." << std::endl;
        return false;
    }
    return it->second->call(request_pdu, timeout_usec);
}

ClientEventType RpcServicesClient::poll(std::string& service_name_out, RpcResponse& response_out) {
    for (auto& entry : rpc_endpoints_) {
        ClientEventType event_type = entry.second->poll(response_out);
        if (event_type != ClientEventType::NONE) {
            service_name_out = entry.first; // service_name is the key
            return event_type;
        }
    }
    return ClientEventType::NONE;
}

bool RpcServicesClient::send_cancel_request(const std::string& service_name) {
    auto it = rpc_endpoints_.find(service_name);
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service '" << service_name << "' not found for sending cancel request." << std::endl;
        return false;
    }
    return it->second->send_cancel_request();
}

void RpcServicesClient::create_request_buffer(const std::string& service_name, PduData& pdu) {
    auto it = rpc_endpoints_.find(service_name);
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service '" << service_name << "' not found for creating request buffer." << std::endl;
        // Should throw an exception or return bool to indicate failure, but void for now.
        return; 
    }
    // Assuming default opcode HAKO_SERVICE_OPERATION_CODE_REQUEST for generic buffer creation
    it->second->create_request_buffer(HAKO_SERVICE_OPERATION_CODE_REQUEST, false, pdu);
}

void RpcServicesClient::create_request_buffer(const std::string& service_name, Hako_uint8 opcode, PduData& pdu) {
    auto it = rpc_endpoints_.find(service_name);
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service '" << service_name << "' not found for creating request buffer." << std::endl;
        // Should throw an exception or return bool to indicate failure, but void for now.
        return; 
    }
    // Determine is_cancel_request based on opcode
    bool is_cancel = (opcode == HAKO_SERVICE_OPERATION_CODE_CANCEL);
    it->second->create_request_buffer(opcode, is_cancel, pdu);
}

} // namespace hakoniwa::pdu::rpc