#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace hakoniwa::pdu::action {

enum class ActionChannelKind : std::uint8_t {
    REQUEST,
    RESPONSE,
    FEEDBACK,
};

struct ActionEndpointReference {
    std::string node_id;
    // Present in generated runtime configuration. User-facing manifests only
    // require nodeId; the generator derives endpointId deterministically.
    std::string endpoint_id;
};

struct ActionChannelDefinition {
    std::size_t slot_index{0};
    ActionChannelKind kind{ActionChannelKind::REQUEST};
    std::uint32_t channel_id{0};
    std::string channel_name;
    std::string packet_type;
};

struct ActionBufferHeap {
    static constexpr std::size_t DEFAULT_SIZE = 1024U * 1024U;

    std::size_t request_size{DEFAULT_SIZE};
    std::size_t response_size{DEFAULT_SIZE};
    std::size_t feedback_size{DEFAULT_SIZE};
};

struct ActionDefinition {
    std::string name;
    std::string type;
    std::size_t slot_count{0};
    ActionBufferHeap buffer_heap;
    ActionEndpointReference client_endpoint;
    ActionEndpointReference server_endpoint;
    std::vector<ActionChannelDefinition> channels;
};

struct ActionConfiguration {
    std::vector<ActionDefinition> actions;
};

class ActionConfigurationLoader {
public:
    static bool load_file(const std::string& path,
                          ActionConfiguration& configuration_out,
                          std::string& error_out);

    static bool parse(const nlohmann::json& root,
                      ActionConfiguration& configuration_out,
                      std::string& error_out);

    static bool parse_action(const nlohmann::json& action,
                             ActionDefinition& definition_out,
                             std::string& error_out);
};

} // namespace hakoniwa::pdu::action
