#include "action_server_endpoint_impl.hpp"

#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/virtual_time_source.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>

namespace action = hakoniwa::pdu::action;

namespace {

nlohmann::json fibonacci_action()
{
    return nlohmann::json::parse(R"({
        "name": "fibonacci",
        "type": "sample_action_msgs/Fibonacci",
        "slotCount": 4,
        "clientEndpoint": {"nodeId": "fibonacci-client"},
        "serverEndpoint": {"nodeId": "fibonacci-server"}
    })");
}

std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint()
{
    return std::make_shared<hakoniwa::pdu::Endpoint>(
        "fibonacci-server", HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
}

std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source()
{
    return std::make_shared<hakoniwa::time_source::VirtualTimeSource>();
}

} // namespace

TEST(ActionServerInitializationContract, AcceptsResolvedLogicalConfiguration)
{
    action::ActionServerEndpointImpl server(
        "fibonacci", 1000, endpoint(), time_source());

    EXPECT_TRUE(server.initialize(
        fibonacci_action(), 24, std::string("fibonacci-client")));
}

TEST(ActionServerInitializationContract, RejectsActionNameMismatch)
{
    action::ActionServerEndpointImpl server(
        "move_robot", 1000, endpoint(), time_source());

    EXPECT_FALSE(server.initialize(fibonacci_action(), 24));
}

TEST(ActionServerInitializationContract, RejectsClientEndpointMismatch)
{
    action::ActionServerEndpointImpl server(
        "fibonacci", 1000, endpoint(), time_source());

    EXPECT_FALSE(server.initialize(
        fibonacci_action(), 24, std::string("another-client")));
}

TEST(ActionServerInitializationContract, RejectsInvalidMetadataSize)
{
    action::ActionServerEndpointImpl server(
        "fibonacci", 1000, endpoint(), time_source());

    EXPECT_FALSE(server.initialize(fibonacci_action(), 0));
}

TEST(ActionServerInitializationContract, RequiresEndpointAndTimeSource)
{
    action::ActionServerEndpointImpl missing_endpoint(
        "fibonacci", 1000, nullptr, time_source());
    action::ActionServerEndpointImpl missing_time_source(
        "fibonacci", 1000, endpoint(), nullptr);

    EXPECT_FALSE(missing_endpoint.initialize(fibonacci_action(), 24));
    EXPECT_FALSE(missing_time_source.initialize(fibonacci_action(), 24));
}
