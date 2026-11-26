#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"
#include "nlohmann/json.hpp"
#include "hakoniwa/time_source/time_source_factory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace hakoniwa::pdu::rpc {


RpcServicesClient::RpcServicesClient(const std::string& node_id, const std::string& client_name,
    const std::string& config_path, const std::string& impl_type,
    uint64_t delta_time_usec, std::string time_source_type,
    std::string endpoint_config_path, std::shared_ptr<CoreProServiceApi> shm_api)
    : node_id_(node_id), client_name_(client_name), config_path_(config_path),
      impl_type_(impl_type), time_source_type_(std::move(time_source_type)),
      endpoint_config_path_(std::move(endpoint_config_path)),
      delta_time_usec_(delta_time_usec), shm_api_(std::move(shm_api)) {
        #ifdef ENABLE_DEBUG_MESSAGES
        std::cout << "DEBUG: node_id_: " << node_id_ << ", client_name_: " << client_name_ << ", config_path_: " << config_path_ << ", impl_type_: " << impl_type_ << ", delta_time_usec_: " << delta_time_usec_ << ", time_source_type: " << time_source_type_ << std::endl;
        #endif
}

// Destructor: ensures all services are stopped cleanly
RpcServicesClient::~RpcServicesClient() {
    stop_all_services();
    if (shm_asset_) shm_asset_->stop();
}

bool RpcServicesClient::initialize_services(std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container) {
    endpoint_container_ = std::move(endpoint_container);
    return initialize_services_impl(false);
}

bool RpcServicesClient::initialize_services_with_endpoint_config() {
    return initialize_services_impl(true);
}

bool RpcServicesClient::initialize_services_impl(bool create_endpoint_container) {
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
            const nlohmann::json* client_endpoint = nullptr;
            
            for (const auto& client_spec : service_entry["clients"]) {
                if (client_spec["name"] == this->client_name_) {
                    client_endpoint = &client_spec.at("client_endpoint");
                    break;
                }
            }

            if (!client_endpoint || client_endpoint->at("nodeId") != node_id_) {
                continue;
            }
            std::cout << "INFO: Initializing RPC client for service: " << service_name << " on node " << this->node_id_ << std::endl;
            std::shared_ptr<IRpcClientEndpoint> rpc_client_endpoint;
            if (client_endpoint->value("protocol", "") == "shm") {
                const auto asset_name = client_endpoint->value("assetName", std::string{});
                const auto core_config = client_endpoint->value("coreConfig", std::string{});
                const auto delta = client_endpoint->value("deltaTimeUsec", delta_time_usec_);
                if (asset_name.empty() || delta == 0) {
                    std::cerr << "ERROR: protocol 'shm' requires assetName and a positive deltaTimeUsec" << std::endl;
                    return false;
                }
                if (!shm_asset_) {
                    shm_asset_ = std::make_shared<ShmRpcAsset>(
                        asset_name, core_config, config_path_, delta,
                        shm_api_ ? shm_api_ : make_core_pro_service_api());
                }
                rpc_client_endpoint = std::make_shared<ShmRpcClientEndpoint>(
                    service_name, client_name_, delta_time_usec_, shm_asset_);
            }
            else {
                const auto client_ep_id = client_endpoint->at("endpointId").get<std::string>();
                if (!endpoint_container_ && create_endpoint_container && !endpoint_config_path_.empty()) {
                    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(node_id_, endpoint_config_path_);
                    if (endpoints->initialize() != HAKO_PDU_ERR_OK) return false;
                    endpoint_container_ = std::move(endpoints);
                }
                if (!endpoint_container_) {
                    std::cerr << "ERROR: PDU Endpoint configuration required for RPC endpoint '" << client_ep_id << "'" << std::endl;
                    return false;
                }
                std::shared_ptr<hakoniwa::pdu::Endpoint> pdu_endpoint = endpoint_container_->ref(client_ep_id);
                if (!pdu_endpoint) {
                    std::cerr << "ERROR: PDU Endpoint instance not found for node '" << node_id_ << "' and endpoint '" << client_ep_id << "'" << std::endl;
                    std::cout.flush();
                    stop_all_services();
                    return false;
                }
                if (!time_source_) {
                    time_source_ = hakoniwa::time_source::create_time_source(time_source_type_, delta_time_usec_);
                }
                rpc_client_endpoint = std::make_shared<RpcClientEndpointImpl>(
                    service_name, client_name_, delta_time_usec_, pdu_endpoint, time_source_);
            }
            
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
