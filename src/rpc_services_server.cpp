#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_server_endpoint_impl.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/real_time_source.hpp"
#include "hakoniwa/time_source/virtual_time_source.hpp"
#include "hakoniwa/time_source/hakoniwa_time_source.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>

namespace hakoniwa::pdu::rpc {

RpcServicesServer::~RpcServicesServer() {
    stop_all_services();
}

bool RpcServicesServer::initialize_services(
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container,
    std::optional<std::string> client_node_id)
{
    return initialize_services_impl(
        std::move(endpoint_container), nullptr, std::move(client_node_id));
}

bool RpcServicesServer::initialize_services(
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
    std::optional<std::string> client_node_id)
{
    return initialize_services_impl(
        nullptr, std::move(endpoint), std::move(client_node_id));
}

bool RpcServicesServer::initialize_services_impl(
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container,
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_override,
    std::optional<std::string> client_node_id)
{
    if (!endpoint_container && !endpoint_override) {
        std::cerr << "ERROR: Endpoint container or endpoint override is required." << std::endl;
        return false;
    }

    endpoint_container_ = std::move(endpoint_container);
    std::cout << "INFO: Initializing RPC Services Server for node: " << node_id_ << std::endl;
    std::cout << "INFO: service_config_path: " << service_config_path_ << std::endl;
    std::filesystem::path file_path(service_config_path_);
    std::filesystem::path parent_abs = std::filesystem::absolute(file_path.parent_path());
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
        int pdu_meta_data_size = json_config.value("pduMetaDataSize", 24);

        for (const auto& service_entry : json_config["services"]) {
            std::string service_name = service_entry["name"];
            std::shared_ptr<hakoniwa::pdu::Endpoint> pdu_endpoint = endpoint_override;

            if (!pdu_endpoint) {
                bool found = false;
                std::string server_endpoint_id;
                if (!service_entry.contains("server_endpoints") || !service_entry["server_endpoints"].is_array()) {
                    std::cerr << "ERROR: 'server_endpoints' section missing or not an array for service " << service_name << std::endl;
                    std::cout.flush();
                    stop_all_services();
                    return false;
                }
                for (const auto& server_ep : service_entry["server_endpoints"]) {
                    if (server_ep["nodeId"] != node_id_) {
                        continue;
                    }
                    server_endpoint_id = server_ep["endpointId"];
                    found = true;
                    break;
                }
                if (!found) {
                    std::cerr << "ERROR: PDU Endpoint not found for service " << service_name
                              << " on node " << node_id_ << ". Check 'server_endpoints' in config." << std::endl;
                    std::cout.flush();
                    stop_all_services();
                    return false;
                }
                pdu_endpoint = endpoint_container_->ref(server_endpoint_id);
                if (!pdu_endpoint) {
                    std::cerr << "ERROR: PDU Endpoint instance not found for service " << service_name
                              << " on node " << node_id_ << " with endpoint " << server_endpoint_id
                              << ". Check endpoint configuration." << std::endl;
                    std::cout.flush();
                    stop_all_services();
                    return false;
                }
            }

            std::shared_ptr<IRpcServerEndpoint> rpc_server_endpoint;
            if (impl_type_ == "RpcServerEndpointImpl") {
                rpc_server_endpoint = std::make_shared<RpcServerEndpointImpl>(
                    service_name, delta_time_usec_, pdu_endpoint, time_source_);
            } else {
                std::cerr << "ERROR: Unsupported RPC Server Endpoint Implementation Type: " << impl_type_ << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }
            if (!rpc_server_endpoint->initialize(service_entry, pdu_meta_data_size, client_node_id)) {
                std::cerr << "ERROR: Failed to initialize RPC server endpoint for service " << service_name << std::endl;
                std::cout.flush();
                stop_all_services();
                return false;
            }

            rpc_endpoints_[service_name] = rpc_server_endpoint;
            std::cout << "INFO: Successfully initialized service: " << service_name
                      << " on node " << node_id_ << std::endl;
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
    return true;
}

void RpcServicesServer::stop_all_services() {
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
