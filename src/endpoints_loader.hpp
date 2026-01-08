#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <stdexcept>
#include <map>
#include <memory>
#include "hakoniwa/pdu/endpoint_types.hpp"
#include "hakoniwa/pdu/endpoint.hpp"

namespace hakoniwa::pdu::rpc {


// Helper function to load endpoints configuration
static inline nlohmann::json load_endpoints_json(const nlohmann::json& json_config, const std::string& parent_abs_path) {
    if (json_config.contains("endpoints_config_path")) {
        std::string endpoints_path = json_config["endpoints_config_path"].get<std::string>();
        std::ifstream ifs(parent_abs_path + "/" + endpoints_path);
        if (!ifs.is_open()) {
            throw std::runtime_error("Failed to open endpoints config file: " + endpoints_path);
        }
        nlohmann::json endpoints_json;
        ifs >> endpoints_json;
        return endpoints_json;
    } else if (json_config.contains("endpoints")) {
        return json_config["endpoints"];
    }
    throw std::runtime_error("Service config missing 'endpoints' or 'endpoints_config_path' section.");
}

static inline bool load_and_initialize_pdu_endpoints(
    const std::string& node_id,
    const nlohmann::json& json_config,
    const std::filesystem::path& parent_abs,
    std::map<std::pair<std::string, std::string>, std::shared_ptr<hakoniwa::pdu::Endpoint>>& pdu_endpoints)
{
    nlohmann::json endpoints_json = load_endpoints_json(json_config, parent_abs.string());
    for (const auto& node_entry : endpoints_json) {
        std::string current_node_id = node_entry["nodeId"];
        if (current_node_id != node_id) {
            continue;
        }

        for (const auto& ep_entry : node_entry["endpoints"]) {
            std::string endpoint_id = ep_entry["id"];
            std::string config_path_for_endpoint = ep_entry["config_path"];

            std::string pdu_endpoint_name = current_node_id + "-" + endpoint_id;
            auto pdu_endpoint = std::make_shared<hakoniwa::pdu::Endpoint>(pdu_endpoint_name, HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
            
            if (pdu_endpoint->open(parent_abs.string() + "/" + config_path_for_endpoint) != HAKO_PDU_ERR_OK) {
                std::cerr << "ERROR: Failed to open PDU endpoint config: " << config_path_for_endpoint << " for node '" << current_node_id << "' endpoint '" << endpoint_id << "'" << std::endl;
                std::cout.flush();
                return false;
            }
            pdu_endpoints[{current_node_id, endpoint_id}] = pdu_endpoint;
            std::cout << "INFO: Successfully initialized PDU endpoint '" << pdu_endpoint_name << "' with config '" << config_path_for_endpoint << "'" << std::endl;
            std::cout.flush();
        }
    }
    return true;
}


static inline std::string find_endpoint_config_path(const nlohmann::json& json_endpoints_config, const std::string& node_id, const std::string& endpoint_id) {
    for (const auto& node_entry : json_endpoints_config) {
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

} // namespace hakoniwa::pdu::rpc