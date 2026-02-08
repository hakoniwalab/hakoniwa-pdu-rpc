#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "nlohmann/json.hpp"
#include "hakoniwa/time_source/time_source_factory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace hakoniwa::pdu::rpc {


// Constructor: takes client_name as a specific identity for this client instance
RpcServicesClient::RpcServicesClient(const std::string& node_id, const std::string& client_name, const std::string& config_path, const std::string& impl_type, uint64_t delta_time_usec, std::string time_source_type)
    : node_id_(node_id), client_name_(client_name), config_path_(config_path), impl_type_(impl_type), delta_time_usec_(delta_time_usec) {
        time_source_ = hakoniwa::time_source::create_time_source(time_source_type, delta_time_usec);
        #ifdef ENABLE_DEBUG_MESSAGES
        std::cout << "DEBUG: node_id_: " << node_id_ << ", client_name_: " << client_name_ << ", config_path_: " << config_path_ << ", impl_type_: " << impl_type_ << ", delta_time_usec_: " << delta_time_usec_ << ", time_source_type: " << time_source_type << std::endl;
        #endif
}

// Destructor: ensures all services are stopped cleanly
RpcServicesClient::~RpcServicesClient() {
    stop_all_services();
}

bool RpcServicesClient::initialize_services(std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container) {
    this->endpoint_container_ = endpoint_container;
    fs::path file_path(this->config_path_);
    fs::path parent_abs = fs::absolute(file_path.parent_path());
    std::cout << "INFO: service_config_path parent: " << parent_abs << std::endl;
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
        int pdu_meta_data_size = json_config.value("pduMetaDataSize", 24);
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
                    #ifdef ENABLE_DEBUG_MESSAGES
                    std::cout << "DEBUG: Found matching client config for service " << service_name << ": nodeId=" << client_ep_node_id << ", endpointId=" << client_ep_id << std::endl;
                    #endif
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
            std::cout << "INFO: Initializing RPC client for service: " << service_name << " on node " << this->node_id_ << std::endl;
            // Create low-level PDU endpoint
            std::shared_ptr<hakoniwa::pdu::Endpoint> pdu_endpoint = endpoint_container_->ref(client_ep_id);
            if (!pdu_endpoint) {
                std::cerr << "ERROR: PDU Endpoint instance not found for node '" << client_ep_node_id << "' and endpoint '" << client_ep_id << "'" << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }
            // Create high-level RPC client endpoint
            std::shared_ptr<IRpcClientEndpoint> rpc_client_endpoint = 
                std::make_shared<RpcClientEndpointImpl>(service_name, client_name_, delta_time_usec_, pdu_endpoint, time_source_);
            
            if (!rpc_client_endpoint->initialize(service_entry, pdu_meta_data_size)) {
                std::cerr << "ERROR: Failed to initialize RPC client endpoint for service " << service_name << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }
            rpc_endpoints_[service_name] = rpc_client_endpoint; // Keyed by service_name
            std::cout << "INFO: Successfully initialized client for service: " << service_name << " on node " << this->node_id_ << std::endl;
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

bool RpcServicesClient::start_all_services() {
    //nothing to do for now
    return true;
}

void RpcServicesClient::stop_all_services() {
    //pdu_endpoints must be stop on caller's responsibility
    for (auto& endpoint_pair : rpc_endpoints_) {
        endpoint_pair.second->clear_pending_responses();
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

ClientEventType RpcServicesClient::poll(std::string& service_name, RpcResponse& response_out) {
    for (auto& entry : rpc_endpoints_) {
        ClientEventType event_type = entry.second->poll(response_out);
        if (event_type != ClientEventType::NONE) {
            service_name = entry.first; // service_name is the key
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

bool RpcServicesClient::create_request_buffer(const std::string& service_name, PduData& pdu) {
    auto it = rpc_endpoints_.find(service_name);
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service '" << service_name << "' not found for creating request buffer." << std::endl;
        return false;
    }
    // Assuming default opcode HAKO_SERVICE_OPERATION_CODE_REQUEST for generic buffer creation
    it->second->create_request_buffer(HAKO_SERVICE_OPERATION_CODE_REQUEST, false, pdu);
    return true;
}

bool RpcServicesClient::create_request_buffer(const std::string& service_name, Hako_uint8 opcode, PduData& pdu) {
    auto it = rpc_endpoints_.find(service_name);
    if (it == rpc_endpoints_.end()) {
        std::cerr << "ERROR: Service '" << service_name << "' not found for creating request buffer." << std::endl;
        return false;
    }
    // Determine is_cancel_request based on opcode
    bool is_cancel = (opcode == HAKO_SERVICE_OPERATION_CODE_CANCEL);
    it->second->create_request_buffer(opcode, is_cancel, pdu);
    return true;
}

void RpcServicesClient::clear_all_instances() {
    for (auto& endpoint_pair : rpc_endpoints_) {
        auto& endpoint = endpoint_pair.second;
        endpoint->clear_pending_responses();
        auto impl = std::dynamic_pointer_cast<RpcClientEndpointImpl>(endpoint);
        if (impl) {
            impl->clear_all_instances();
        }
    }
}

} // namespace hakoniwa::pdu::rpc
