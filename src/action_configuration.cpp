#include "action_configuration.hpp"

#include <fstream>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

namespace hakoniwa::pdu::action {
namespace {

bool read_required_text(const nlohmann::json& object,
                        const char* key,
                        std::string& value_out,
                        std::string& error_out)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        error_out = std::string("'") + key + "' must be a non-empty string";
        return false;
    }
    value_out = it->get<std::string>();
    return true;
}

bool read_endpoint(const nlohmann::json& action,
                   const char* key,
                   ActionEndpointReference& endpoint_out,
                   std::string& error_out)
{
    const auto it = action.find(key);
    if (it == action.end() || !it->is_object()) {
        error_out = std::string("'") + key + "' must be an object";
        return false;
    }
    return read_required_text(*it, "nodeId", endpoint_out.node_id, error_out);
}

std::string packet_type(const std::string& action_type, const char* suffix)
{
    return action_type + "Action" + suffix;
}

void append_slot_channels(ActionDefinition& definition, std::size_t slot)
{
    const auto base_id = static_cast<std::uint32_t>(slot * 3U);
    const auto slot_name = std::string("Slot") + std::to_string(slot);
    definition.channels.push_back(ActionChannelDefinition{
        slot,
        ActionChannelKind::REQUEST,
        base_id,
        slot_name + "Request",
        packet_type(definition.type, "Request"),
    });
    definition.channels.push_back(ActionChannelDefinition{
        slot,
        ActionChannelKind::RESPONSE,
        base_id + 1U,
        slot_name + "Response",
        packet_type(definition.type, "Response"),
    });
    definition.channels.push_back(ActionChannelDefinition{
        slot,
        ActionChannelKind::FEEDBACK,
        base_id + 2U,
        slot_name + "Feedback",
        packet_type(definition.type, "Feedback"),
    });
}

} // namespace

bool ActionConfigurationLoader::load_file(
    const std::string& path,
    ActionConfiguration& configuration_out,
    std::string& error_out)
{
    std::ifstream stream(path);
    if (!stream.is_open()) {
        error_out = "failed to open Action configuration: " + path;
        return false;
    }

    try {
        nlohmann::json root;
        stream >> root;
        return parse(root, configuration_out, error_out);
    } catch (const nlohmann::json::exception& error) {
        error_out = std::string("failed to parse Action configuration JSON: ") + error.what();
        return false;
    }
}

bool ActionConfigurationLoader::parse(
    const nlohmann::json& root,
    ActionConfiguration& configuration_out,
    std::string& error_out)
{
    if (!root.is_object()) {
        error_out = "Action configuration root must be an object";
        return false;
    }

    ActionConfiguration parsed;
    const auto actions = root.find("actions");
    if (actions == root.end() || !actions->is_array() || actions->empty()) {
        error_out = "'actions' must be a non-empty array";
        return false;
    }

    std::set<std::string> names;
    for (const auto& action : *actions) {
        ActionDefinition definition;
        std::string action_error;
        if (!parse_action(action, definition, action_error)) {
            error_out = "invalid Action definition: " + action_error;
            return false;
        }
        if (!names.insert(definition.name).second) {
            error_out = "duplicate Action name: " + definition.name;
            return false;
        }
        parsed.actions.push_back(std::move(definition));
    }

    configuration_out = std::move(parsed);
    error_out.clear();
    return true;
}

bool ActionConfigurationLoader::parse_action(
    const nlohmann::json& action,
    ActionDefinition& definition_out,
    std::string& error_out)
{
    if (!action.is_object()) {
        error_out = "Action entry must be an object";
        return false;
    }

    ActionDefinition parsed;
    if (!read_required_text(action, "name", parsed.name, error_out)
        || !read_required_text(action, "type", parsed.type, error_out)) {
        return false;
    }
    const auto separator = parsed.type.find('/');
    if (separator == std::string::npos || separator == 0
        || separator + 1 == parsed.type.size()
        || parsed.type.find('/', separator + 1) != std::string::npos) {
        error_out = "'type' must use package/ActionName format";
        return false;
    }

    const auto slot_count = action.find("slotCount");
    if (slot_count == action.end() || !slot_count->is_number_unsigned()) {
        error_out = "'slotCount' must be a positive integer";
        return false;
    }
    parsed.slot_count = slot_count->get<std::size_t>();
    if (parsed.slot_count == 0) {
        error_out = "'slotCount' must be a positive integer";
        return false;
    }
    constexpr auto max_slot_count =
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max() - 2U) / 3U) + 1U;
    if (parsed.slot_count > max_slot_count) {
        error_out = "'slotCount' exceeds the logical channel ID range";
        return false;
    }

    if (!read_endpoint(action, "clientEndpoint", parsed.client_endpoint, error_out)
        || !read_endpoint(action, "serverEndpoint", parsed.server_endpoint, error_out)) {
        return false;
    }

    parsed.channels.reserve(parsed.slot_count * 3U);
    for (std::size_t slot = 0; slot < parsed.slot_count; ++slot) {
        append_slot_channels(parsed, slot);
    }

    definition_out = std::move(parsed);
    error_out.clear();
    return true;
}

} // namespace hakoniwa::pdu::action
