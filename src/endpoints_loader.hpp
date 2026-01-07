#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <stdexcept>

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