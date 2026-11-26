#include "rpc_bridge.hpp"

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_client_endpoint_impl.hpp"
#include "hakoniwa/pdu/rpc/rpc_server_endpoint_impl.hpp"
#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using hakoniwa::pdu::Endpoint;
using hakoniwa::pdu::EndpointContainer;
using namespace hakoniwa::pdu::rpc;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int)
{
    stop_requested = 1;
}

struct Route {
    std::shared_ptr<IRpcServerEndpoint> server;
    std::shared_ptr<IRpcClientEndpoint> client;
    RpcBridge bridge{*server, *client};
};

int run(const fs::path& config_path, const std::string& node)
{
    std::ifstream input(config_path);
    if (!input) throw std::runtime_error("Cannot open RPC definition: " + config_path.string());
    const auto definition = json::parse(input);
    const auto metadata_size = definition.value("pduMetaDataSize", 24);
    if (!definition.at("services").is_array()) throw std::runtime_error("Expected a services array");

    std::unique_ptr<EndpointContainer> container;
    const auto transport = [&](const json& ref) -> std::shared_ptr<Endpoint> {
        const auto id = ref.at("endpointId").get<std::string>();
        if (!container) {
            const auto endpoints = config_path.parent_path() / "endpoints.json";
            container = std::make_unique<EndpointContainer>(node, endpoints.string());
            if (container->initialize() != HAKO_PDU_ERR_OK) {
                throw std::runtime_error(container->last_error());
            }
        }
        auto endpoint = container->ref(id);
        if (!endpoint) throw std::runtime_error("Missing PDU endpoint: " + id);
        return endpoint;
    };

    using AssetConfig = std::tuple<std::string, std::string, uint64_t>;
    std::optional<AssetConfig> asset_config;
    std::shared_ptr<ShmRpcAsset> asset;
    std::set<std::string> services;
    std::vector<Route> routes;
    for (const auto& service : definition.at("services")) {
        const auto name = service.at("name").get<std::string>();
        const json* server_ref = nullptr;
        const json* outgoing = nullptr;
        auto incoming_service = service;
        incoming_service["clients"] = json::array();
        const auto server_specs = service.value("server_endpoints", json::array());
        const auto client_specs = service.value("clients", json::array());
        for (const auto& server : server_specs) {
            if (server.at("nodeId") != node) continue;
            if (server_ref) throw std::runtime_error("Duplicate bridge server for " + name);
            server_ref = &server;
        }
        for (const auto& client : client_specs) {
            const auto& ref = client.at("client_endpoint");
            if (ref.at("nodeId") == node) {
                if (outgoing) throw std::runtime_error("Duplicate bridge client for " + name);
                outgoing = &client;
            } else if (ref.value("protocol", "") != "shm") {
                incoming_service["clients"].push_back(client);
            }
        }
        if (!server_ref && !outgoing) continue;
        if (!server_ref || !outgoing || !services.insert(name).second) {
            throw std::runtime_error("Expected one bridge server and one bridge client for " + name);
        }
        if (server_ref->value("protocol", "") == "shm") {
            throw std::runtime_error("SHM RPC server is not supported for " + name);
        }
        const auto server = std::make_shared<RpcServerEndpointImpl>(name, 0, transport(*server_ref), nullptr);
        if (!server->initialize(incoming_service, metadata_size)) {
            throw std::runtime_error("Failed to initialize bridge server for " + name);
        }

        std::shared_ptr<IRpcClientEndpoint> client;
        const auto client_name = outgoing->at("name").get<std::string>();
        const auto& ref = outgoing->at("client_endpoint");
        if (ref.value("protocol", "") == "shm") {
            const auto step = ref.value("deltaTimeUsec", int64_t{1000});
            const auto asset_name = ref.at("assetName").get<std::string>();
            const auto core_config = ref.value("coreConfig", std::string{});
            if (asset_name.empty() || step <= 0) {
                throw std::runtime_error("SHM requires assetName and a positive deltaTimeUsec for " + name);
            }
            const AssetConfig requested{asset_name, core_config, static_cast<uint64_t>(step)};
            if (!asset) {
                asset_config = requested;
                asset = std::make_shared<ShmRpcAsset>(asset_name, core_config, config_path.string(), step);
            } else if (asset_config != requested) {
                throw std::runtime_error("All SHM routes must share the same asset configuration");
            }
            auto shm = std::make_shared<ShmRpcClientEndpoint>(name, client_name, step, asset);
            if (!shm->initialize_without_start(service, metadata_size)) {
                throw std::runtime_error("Failed to initialize bridge client for " + name);
            }
            client = std::move(shm);
        } else {
            client = std::make_shared<RpcClientEndpointImpl>(name, client_name, 0, transport(ref), nullptr);
            if (!client->initialize(service, metadata_size)) {
                throw std::runtime_error("Failed to initialize bridge client for " + name);
            }
        }
        routes.push_back({server, client});
    }
    if (routes.empty()) throw std::runtime_error("No services configured for bridge node " + node);

    if (asset && !asset->start()) throw std::runtime_error("Failed to start bridge SHM asset");
    if (container->start_all() != HAKO_PDU_ERR_OK) {
        container->stop_all();
        throw std::runtime_error("Failed to start bridge PDU endpoints");
    }
    std::cout << "rpc-bridge ready: " << node << " (" << routes.size() << " services)" << std::endl;
    while (!stop_requested) {
        for (auto& route : routes) route.bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    container->stop_all();
    if (asset) asset->stop();
    for (const auto& route : routes) {
        if (auto client = std::dynamic_pointer_cast<RpcClientEndpointImpl>(route.client)) {
            client->clear_all_instances();
        }
    }
    RpcServerEndpointImpl::clear_all_instances();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    fs::path config_path;
    std::string node;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--rpc-config") config_path = argv[i + 1];
        if (option == "--node") node = argv[i + 1];
    }
    if (argc != 5 || config_path.empty() || node.empty()) {
        std::cerr << "Usage: rpc-bridge --rpc-config <RPC definition> --node <bridge node id>\n";
        return 2;
    }
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    try {
        return run(fs::absolute(config_path), node);
    } catch (const std::exception& error) {
        std::cerr << "rpc-bridge: " << error.what() << '\n';
        return 1;
    }
}
