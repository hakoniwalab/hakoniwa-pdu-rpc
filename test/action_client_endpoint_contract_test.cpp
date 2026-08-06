#include "action_client_endpoint_impl.hpp"

#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/virtual_time_source.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <vector>

namespace action = hakoniwa::pdu::action;

namespace {

nlohmann::json fibonacci_action(std::size_t slot_count = 1)
{
    return {
        {"name", "fibonacci"},
        {"type", "sample_action_msgs/Fibonacci"},
        {"slotCount", slot_count},
        {"bufferHeap", {
            {"requestSize", 64},
            {"responseSize", 64},
            {"feedbackSize", 64},
        }},
        {"clientEndpoint", {{"nodeId", "fibonacci-client"}}},
        {"serverEndpoint", {{"nodeId", "fibonacci-server"}}},
    };
}

std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint()
{
    return std::make_shared<hakoniwa::pdu::Endpoint>(
        "fibonacci-client", HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
}

std::shared_ptr<action::ActionClientEndpointImpl> client(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& action_endpoint,
    const std::shared_ptr<hakoniwa::time_source::VirtualTimeSource>& clock)
{
    return std::make_shared<action::ActionClientEndpointImpl>(
        "fibonacci", "test-client", 1000, action_endpoint, clock);
}

action::GoalId goal_id(std::uint8_t seed)
{
    action::GoalId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::uint8_t>(seed + index);
    }
    return id;
}

action::PduData encoded_goal(
    const std::shared_ptr<action::ActionClientEndpointImpl>& action_client,
    std::int32_t order = 8)
{
    action::PduData packet;
    EXPECT_TRUE(action_client->create_goal_buffer(packet));
    HakoCpp_FibonacciActionRequest request{};
    request.body.order = order;
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
    const int encoded_size = convertor.cpp2pdu(
        request,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(encoded_size, 0);
    return packet;
}

action::PduData response_packet(
    const action::GoalId& id,
    std::uint8_t response_kind,
    std::uint8_t status)
{
    HakoCpp_FibonacciActionResponse response{};
    response.header.version = 1;
    response.header.response_kind = response_kind;
    response.header.status = status;
    response.header.goal_id = id;

    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    action::PduData packet(1024, 0);
    const int encoded_size = convertor.cpp2pdu(
        response,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(encoded_size, 0);
    if (encoded_size > 0) {
        packet.resize(static_cast<std::size_t>(encoded_size));
    }
    return packet;
}

action::PduData goal_response(
    const action::GoalId& id,
    action::Decision decision)
{
    return response_packet(
        id, 1, static_cast<std::uint8_t>(decision));
}

action::PduData cancel_response(
    const action::GoalId& id,
    action::Decision decision)
{
    return response_packet(
        id, 2, static_cast<std::uint8_t>(decision));
}

action::PduData feedback_packet(
    const action::GoalId& id,
    std::uint32_t sequence,
    std::vector<std::int32_t> partial_sequence)
{
    HakoCpp_FibonacciActionFeedback feedback{};
    feedback.header.version = 1;
    feedback.header.goal_id = id;
    feedback.header.sequence_no = sequence;
    feedback.body.partial_sequence = std::move(partial_sequence);

    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
    action::PduData packet(1024, 0);
    const int encoded_size = convertor.cpp2pdu(
        feedback,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(encoded_size, 0);
    if (encoded_size > 0) {
        packet.resize(static_cast<std::size_t>(encoded_size));
    }
    return packet;
}

action::PduData result_packet(
    const action::GoalId& id,
    action::TerminalStatus status,
    std::vector<std::int32_t> sequence)
{
    HakoCpp_FibonacciActionResponse response{};
    response.header.version = 1;
    response.header.response_kind = 3;
    response.header.status = static_cast<std::uint8_t>(status);
    response.header.goal_id = id;
    response.body.sequence = std::move(sequence);

    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    action::PduData packet(1024, 0);
    const int encoded_size = convertor.cpp2pdu(
        response,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(encoded_size, 0);
    if (encoded_size > 0) {
        packet.resize(static_cast<std::size_t>(encoded_size));
    }
    return packet;
}

class ActionClientFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        action_endpoint = endpoint();
        ASSERT_EQ(
            action_endpoint->open(ACTION_CLIENT_ENDPOINT_FIXTURE_PATH),
            HAKO_PDU_ERR_OK);
        ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);
        clock = std::make_shared<hakoniwa::time_source::VirtualTimeSource>();
        action_client = client(action_endpoint, clock);
    }

    void TearDown() override
    {
        EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
    }

    std::shared_ptr<hakoniwa::pdu::Endpoint> action_endpoint;
    std::shared_ptr<hakoniwa::time_source::VirtualTimeSource> clock;
    std::shared_ptr<action::ActionClientEndpointImpl> action_client;
};

} // namespace

TEST_F(ActionClientFixture, InitializesAndCreatesGoalEncodingBuffer)
{
    action::PduData packet;
    EXPECT_FALSE(action_client->create_goal_buffer(packet));
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    EXPECT_FALSE(action_client->initialize(fibonacci_action()));
    ASSERT_TRUE(action_client->create_goal_buffer(packet));

    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, packet.data(), sizeof(metadata));
    EXPECT_EQ(metadata.total_size, packet.size());
    EXPECT_EQ(metadata.total_size - metadata.heap_off, 64U);
}

TEST_F(ActionClientFixture, PreservesCallerGoalIdAndWritesRequestHeader)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0x10);
    const auto packet = encoded_goal(action_client, 13);

    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(packet, id, handle, 5000));
    EXPECT_EQ(handle.goal_id, id);

    action::PduData received(1024, 0);
    std::size_t received_size = 0;
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->recv(
            request_key,
            std::as_writable_bytes(std::span(received)),
            received_size),
        HAKO_PDU_ERR_OK);
    ASSERT_GT(received_size, 0U);
    received.resize(received_size);

    HakoCpp_FibonacciActionRequest request{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
    ASSERT_TRUE(convertor.pdu2cpp(
        reinterpret_cast<char*>(received.data()), request));
    EXPECT_EQ(request.header.version, 1);
    EXPECT_EQ(request.header.request_kind, 1);
    EXPECT_EQ(request.header.goal_id, id);
    EXPECT_EQ(request.body.order, 13);
}

TEST_F(ActionClientFixture, RejectsInvalidDuplicateAndExhaustedGoalsSynchronously)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto packet = encoded_goal(action_client);
    const auto first_id = goal_id(0x20);
    const auto second_id = goal_id(0x40);

    action::ClientGoalHandle handle;
    EXPECT_FALSE(action_client->send_goal(packet, action::GoalId{}, handle));
    ASSERT_TRUE(action_client->send_goal(packet, first_id, handle));
    EXPECT_FALSE(action_client->send_goal(packet, first_id, handle));
    EXPECT_FALSE(action_client->send_goal(packet, second_id, handle));
}

TEST_F(ActionClientFixture, RejectedGoalReleasesSlot)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto first_id = goal_id(0x30);
    const auto second_id = goal_id(0x50);
    const auto packet = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(packet, first_id, handle));

    const auto response = goal_response(first_id, action::Decision::REJECTED);
    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    ASSERT_EQ(
        action_endpoint->send(response_key, std::as_bytes(std::span(response))),
        HAKO_PDU_ERR_OK);

    action::ClientEvent event;
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(event.goal.goal_id, first_id);
    EXPECT_EQ(event.decision, action::Decision::REJECTED);
    EXPECT_TRUE(action_client->send_goal(packet, second_id, handle));
}

TEST_F(ActionClientFixture, AcceptedGoalRetainsSlotUntilTerminalResult)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto first_id = goal_id(0x60);
    const auto second_id = goal_id(0x70);
    const auto packet = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(packet, first_id, handle));

    const auto response = goal_response(first_id, action::Decision::ACCEPTED);
    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    ASSERT_EQ(
        action_endpoint->send(response_key, std::as_bytes(std::span(response))),
        HAKO_PDU_ERR_OK);

    action::ClientEvent event;
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(event.decision, action::Decision::ACCEPTED);
    EXPECT_FALSE(action_client->send_goal(packet, second_id, handle));
}

TEST_F(ActionClientFixture, GoalResponseTimeoutEmitsOnceAndQuarantinesSlot)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto first_id = goal_id(0x80);
    const auto second_id = goal_id(0x90);
    const auto packet = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(packet, first_id, handle, 1000));

    action::ClientEvent event;
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
    clock->advance_time(999);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
    clock->advance_time(1);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::TIMEOUT);
    EXPECT_EQ(event.goal.goal_id, first_id);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
    EXPECT_FALSE(action_client->send_goal(packet, second_id, handle, 1000));

    action_client->reset_contexts();
    EXPECT_TRUE(action_client->send_goal(packet, second_id, handle, 1000));
}

TEST_F(ActionClientFixture, ZeroTimeoutDoesNotExpireGoalResponseWait)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto packet = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(packet, goal_id(0xa0), handle, 0));

    clock->advance_time(1000000);
    action::ClientEvent event;
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
}

TEST_F(ActionClientFixture, IgnoresResponseForUnknownGoal)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto response = goal_response(
        goal_id(0xb0), action::Decision::ACCEPTED);
    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    ASSERT_EQ(
        action_endpoint->send(response_key, std::as_bytes(std::span(response))),
        HAKO_PDU_ERR_OK);

    action::ClientEvent event;
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
}

TEST_F(ActionClientFixture, DeliversAcceptedGoalFeedbackInSequence)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0xb0);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));

    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);

    const hakoniwa::pdu::PduResolvedKey feedback_key{"fibonacci", 2};
    const auto first = feedback_packet(id, 0, {0, 1, 1});
    ASSERT_EQ(
        action_endpoint->send(
            feedback_key, std::as_bytes(std::span(first))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_client->poll(event), action::ClientEventType::FEEDBACK);
    EXPECT_EQ(event.goal.goal_id, id);
    EXPECT_EQ(event.feedback_sequence, 0U);

    HakoCpp_FibonacciActionFeedback decoded{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
    ASSERT_TRUE(convertor.pdu2cpp(
        reinterpret_cast<char*>(event.pdu.data()), decoded));
    EXPECT_EQ(decoded.body.partial_sequence, (std::vector<std::int32_t>{0, 1, 1}));

    const auto out_of_order = feedback_packet(id, 2, {0, 1, 1, 2, 3});
    ASSERT_EQ(
        action_endpoint->send(
            feedback_key, std::as_bytes(std::span(out_of_order))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);

    const auto second = feedback_packet(id, 1, {0, 1, 1, 2});
    ASSERT_EQ(
        action_endpoint->send(
            feedback_key, std::as_bytes(std::span(second))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_client->poll(event), action::ClientEventType::FEEDBACK);
    EXPECT_EQ(event.feedback_sequence, 1U);

    ASSERT_EQ(
        action_endpoint->send(
            feedback_key, std::as_bytes(std::span(second))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
}

TEST_F(ActionClientFixture, DeliversTerminalResultAndReleasesSlot)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0xd0);
    const auto next_id = goal_id(0xe0);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));

    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);

    const auto result = result_packet(
        id, action::TerminalStatus::SUCCEEDED, {0, 1, 1, 2, 3, 5});
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(result))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_client->poll(event), action::ClientEventType::RESULT);
    EXPECT_EQ(event.goal.goal_id, id);
    EXPECT_EQ(event.terminal_status, action::TerminalStatus::SUCCEEDED);

    HakoCpp_FibonacciActionResponse decoded{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    ASSERT_TRUE(convertor.pdu2cpp(
        reinterpret_cast<char*>(event.pdu.data()), decoded));
    EXPECT_EQ(
        decoded.body.sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2, 3, 5}));

    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(result))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
    EXPECT_TRUE(action_client->send_goal(goal, next_id, handle));
}

TEST_F(ActionClientFixture, DeliversAbortedResultAndReleasesSlot)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0xd8);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));

    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);

    const auto aborted = result_packet(
        id, action::TerminalStatus::ABORTED, {0, 1});
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(aborted))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_client->poll(event), action::ClientEventType::RESULT);
    EXPECT_EQ(event.goal.goal_id, id);
    EXPECT_EQ(event.terminal_status, action::TerminalStatus::ABORTED);
    EXPECT_TRUE(action_client->send_goal(
        goal, goal_id(0xe8), handle));
}

TEST_F(ActionClientFixture, SendsCancelAndAcceptsCanceledResult)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0x18);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    EXPECT_FALSE(action_client->send_cancel(handle));
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));
    EXPECT_FALSE(action_client->send_cancel(handle));

    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    action::PduData initial_request(1024, 0);
    std::size_t initial_request_size = 0;
    ASSERT_EQ(
        action_endpoint->recv(
            request_key,
            std::as_writable_bytes(std::span(initial_request)),
            initial_request_size),
        HAKO_PDU_ERR_OK);

    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);
    ASSERT_TRUE(action_client->send_cancel(handle));
    EXPECT_FALSE(action_client->send_cancel(handle));

    action::PduData request(1024, 0);
    std::size_t request_size = 0;
    ASSERT_EQ(
        action_endpoint->recv(
            request_key,
            std::as_writable_bytes(std::span(request)),
            request_size),
        HAKO_PDU_ERR_OK);
    request.resize(request_size);
    HakoCpp_FibonacciActionRequest decoded_request{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest request_convertor;
    ASSERT_TRUE(request_convertor.pdu2cpp(
        reinterpret_cast<char*>(request.data()), decoded_request));
    EXPECT_EQ(decoded_request.header.request_kind, 2);
    EXPECT_EQ(decoded_request.header.goal_id, id);
    EXPECT_EQ(decoded_request.body.order, 0);

    const auto cancel_accepted = cancel_response(
        id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(cancel_accepted))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::CANCEL_RESPONSE);
    EXPECT_EQ(event.goal.goal_id, id);
    EXPECT_EQ(event.decision, action::Decision::ACCEPTED);

    const auto canceled = result_packet(
        id, action::TerminalStatus::CANCELED, {0, 1});
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(canceled))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_client->poll(event), action::ClientEventType::RESULT);
    EXPECT_EQ(event.terminal_status, action::TerminalStatus::CANCELED);
    EXPECT_TRUE(action_client->send_goal(
        goal, goal_id(0x28), handle));
}

TEST_F(ActionClientFixture, CancelRejectedReturnsToAcceptedState)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0x38);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));
    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);
    ASSERT_TRUE(action_client->send_cancel(handle));

    const auto rejected = cancel_response(id, action::Decision::REJECTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(rejected))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::CANCEL_RESPONSE);
    EXPECT_EQ(event.decision, action::Decision::REJECTED);
    EXPECT_TRUE(action_client->send_cancel(handle));
}

TEST_F(ActionClientFixture, ResultWinsWhileCancelResponseIsPending)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0x48);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));
    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);
    ASSERT_TRUE(action_client->send_cancel(handle));

    const auto succeeded = result_packet(
        id, action::TerminalStatus::SUCCEEDED, {0, 1, 1});
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(succeeded))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_client->poll(event), action::ClientEventType::RESULT);
    EXPECT_EQ(event.terminal_status, action::TerminalStatus::SUCCEEDED);

    const auto late_cancel = cancel_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(late_cancel))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
}

TEST_F(ActionClientFixture, IgnoresInvalidResultStatusAndRetainsSlot)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto id = goal_id(0xf0);
    const auto goal = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(goal, id, handle));

    const hakoniwa::pdu::PduResolvedKey response_key{"fibonacci", 1};
    const auto accepted = goal_response(id, action::Decision::ACCEPTED);
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(accepted))),
        HAKO_PDU_ERR_OK);
    action::ClientEvent event;
    ASSERT_EQ(
        action_client->poll(event),
        action::ClientEventType::GOAL_RESPONSE);

    const auto invalid = result_packet(
        id, action::TerminalStatus::UNSPECIFIED, {});
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(invalid))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
    const auto canceled = result_packet(
        id, action::TerminalStatus::CANCELED, {});
    ASSERT_EQ(
        action_endpoint->send(
            response_key, std::as_bytes(std::span(canceled))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_client->poll(event), action::ClientEventType::NONE);
    EXPECT_FALSE(action_client->send_goal(
        goal, goal_id(0x20), handle));
}

TEST_F(ActionClientFixture, ClearPendingEventsDoesNotReleaseActiveContext)
{
    ASSERT_TRUE(action_client->initialize(fibonacci_action()));
    const auto packet = encoded_goal(action_client);
    action::ClientGoalHandle handle;
    ASSERT_TRUE(action_client->send_goal(packet, goal_id(0xc0), handle));

    action_client->clear_pending_events();
    EXPECT_FALSE(action_client->send_goal(
        packet, goal_id(0xd0), handle));

    action_client->reset_contexts();
    EXPECT_TRUE(action_client->send_goal(
        packet, goal_id(0xd0), handle));
}
