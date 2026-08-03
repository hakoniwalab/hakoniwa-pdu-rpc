#include "action_configuration.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace action = hakoniwa::pdu::action;

TEST(ActionConfigurationContract, LoadsAndExpandsSampleConfiguration)
{
    action::ActionConfiguration configuration;
    std::string error;

    ASSERT_TRUE(action::ActionConfigurationLoader::load_file(
        ACTION_CONFIG_FIXTURE_PATH, configuration, error)) << error;
    ASSERT_EQ(configuration.actions.size(), 1U);

    const auto& definition = configuration.actions.front();
    EXPECT_EQ(definition.name, "fibonacci");
    EXPECT_EQ(definition.type, "sample_action_msgs/Fibonacci");
    EXPECT_EQ(definition.slot_count, 4U);
    EXPECT_EQ(definition.client_endpoint.node_id, "fibonacci-client");
    EXPECT_EQ(definition.server_endpoint.node_id, "fibonacci-server");
    ASSERT_EQ(definition.channels.size(), 12U);

    const auto& first = definition.channels.front();
    EXPECT_EQ(first.slot_index, 0U);
    EXPECT_EQ(first.kind, action::ActionChannelKind::REQUEST);
    EXPECT_EQ(first.channel_id, 0U);
    EXPECT_EQ(first.channel_name, "Slot0Request");
    EXPECT_EQ(first.packet_type, "sample_action_msgs/FibonacciActionRequest");

    const auto& last = definition.channels.back();
    EXPECT_EQ(last.slot_index, 3U);
    EXPECT_EQ(last.kind, action::ActionChannelKind::FEEDBACK);
    EXPECT_EQ(last.channel_id, 11U);
    EXPECT_EQ(last.channel_name, "Slot3Feedback");
    EXPECT_EQ(last.packet_type, "sample_action_msgs/FibonacciActionFeedback");
}

TEST(ActionConfigurationContract, RejectsInvalidSlotCount)
{
    const auto action_json = nlohmann::json::parse(R"({
        "name": "fibonacci",
        "type": "sample_action_msgs/Fibonacci",
        "slotCount": 0,
        "clientEndpoint": {"nodeId": "client"},
        "serverEndpoint": {"nodeId": "server"}
    })");
    action::ActionDefinition definition;
    std::string error;

    EXPECT_FALSE(action::ActionConfigurationLoader::parse_action(
        action_json, definition, error));
    EXPECT_NE(error.find("slotCount"), std::string::npos);
}

TEST(ActionConfigurationContract, RejectsMalformedActionType)
{
    const auto action_json = nlohmann::json::parse(R"({
        "name": "fibonacci",
        "type": "Fibonacci",
        "slotCount": 1,
        "clientEndpoint": {"nodeId": "client"},
        "serverEndpoint": {"nodeId": "server"}
    })");
    action::ActionDefinition definition;
    std::string error;

    EXPECT_FALSE(action::ActionConfigurationLoader::parse_action(
        action_json, definition, error));
    EXPECT_NE(error.find("package/ActionName"), std::string::npos);
}

TEST(ActionConfigurationContract, RejectsDuplicateActionNames)
{
    const auto root = nlohmann::json::parse(R"({
        "actions": [
            {
                "name": "fibonacci",
                "type": "sample_action_msgs/Fibonacci",
                "slotCount": 1,
                "clientEndpoint": {"nodeId": "client-a"},
                "serverEndpoint": {"nodeId": "server"}
            },
            {
                "name": "fibonacci",
                "type": "sample_action_msgs/Fibonacci",
                "slotCount": 1,
                "clientEndpoint": {"nodeId": "client-b"},
                "serverEndpoint": {"nodeId": "server"}
            }
        ]
    })");
    action::ActionConfiguration configuration;
    std::string error;

    EXPECT_FALSE(action::ActionConfigurationLoader::parse(
        root, configuration, error));
    EXPECT_NE(error.find("duplicate Action name"), std::string::npos);
}
