#include "action_server_endpoint_impl.hpp"

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

#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace action = hakoniwa::pdu::action;

namespace {

class ResponseFailingEndpoint final : public hakoniwa::pdu::Endpoint {
public:
    ResponseFailingEndpoint()
        : Endpoint(
            "fibonacci-server",
            HAKO_PDU_ENDPOINT_DIRECTION_INOUT)
    {
    }

    HakoPduErrorType send(
        const hakoniwa::pdu::PduResolvedKey& key,
        std::span<const std::byte> data) noexcept override
    {
        if (key.channel_id % 3 == 1) {
            return HAKO_PDU_ERR_IO_ERROR;
        }
        return Endpoint::send(key, data);
    }
};

class FeedbackFailingOnceEndpoint final : public hakoniwa::pdu::Endpoint {
public:
    FeedbackFailingOnceEndpoint()
        : Endpoint(
            "fibonacci-server",
            HAKO_PDU_ENDPOINT_DIRECTION_INOUT)
    {
    }

    HakoPduErrorType send(
        const hakoniwa::pdu::PduResolvedKey& key,
        std::span<const std::byte> data) noexcept override
    {
        if (fail_feedback_ && key.channel_id % 3 == 2) {
            fail_feedback_ = false;
            return HAKO_PDU_ERR_IO_ERROR;
        }
        return Endpoint::send(key, data);
    }

private:
    bool fail_feedback_{true};
};

class ResultFailingOnceEndpoint final : public hakoniwa::pdu::Endpoint {
public:
    ResultFailingOnceEndpoint()
        : Endpoint(
            "fibonacci-server",
            HAKO_PDU_ENDPOINT_DIRECTION_INOUT)
    {
    }

    HakoPduErrorType send(
        const hakoniwa::pdu::PduResolvedKey& key,
        std::span<const std::byte> data) noexcept override
    {
        if (key.channel_id % 3 == 1
            && ++response_send_count_ == 2) {
            return HAKO_PDU_ERR_IO_ERROR;
        }
        return Endpoint::send(key, data);
    }

private:
    std::size_t response_send_count_{0};
};

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

std::shared_ptr<action::ActionServerEndpointImpl> server(
    std::shared_ptr<hakoniwa::pdu::Endpoint> action_endpoint = endpoint())
{
    return std::make_shared<action::ActionServerEndpointImpl>(
        "fibonacci", 1000, std::move(action_endpoint), time_source());
}

const action::GoalId kGoalId{
    0x10, 0x11, 0x12, 0x13,
    0x20, 0x21, 0x22, 0x23,
    0x30, 0x31, 0x32, 0x33,
    0x40, 0x41, 0x42, 0x43,
};

action::PduData fibonacci_goal_request(
    std::uint8_t version = 1,
    std::uint8_t request_kind = 1,
    action::GoalId goal_id = kGoalId)
{
    HakoCpp_FibonacciActionRequest source{};
    source.header.version = version;
    source.header.request_kind = request_kind;
    source.header.goal_id = goal_id;
    source.body.order = 8;

    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
    action::PduData packet(1024, 0);
    const int size = convertor.cpp2pdu(
        source,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(size, 0);
    if (size > 0) {
        packet.resize(static_cast<std::size_t>(size));
    }
    return packet;
}

action::PduData with_extra_heap(
    action::PduData packet,
    std::size_t extra_size)
{
    packet.resize(packet.size() + extra_size, 0);
    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, packet.data(), sizeof(metadata));
    metadata.total_size = static_cast<std::uint32_t>(packet.size());
    std::memcpy(packet.data(), &metadata, sizeof(metadata));
    return packet;
}

HakoCpp_FibonacciActionResponse receive_fibonacci_response(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& action_endpoint,
    HakoPduChannelIdType channel_id,
    std::size_t* received_size_out = nullptr)
{
    action::PduData packet(1024, 0);
    std::size_t received_size = 0;
    const hakoniwa::pdu::PduResolvedKey response_key{
        "fibonacci",
        channel_id,
    };
    EXPECT_EQ(
        action_endpoint->recv(
            response_key,
            std::as_writable_bytes(std::span(packet)),
            received_size),
        HAKO_PDU_ERR_OK);
    EXPECT_GT(received_size, 0U);
    if (received_size_out != nullptr) {
        *received_size_out = received_size;
    }
    packet.resize(received_size);

    HakoCpp_FibonacciActionResponse response{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    EXPECT_TRUE(convertor.pdu2cpp(
        reinterpret_cast<char*>(packet.data()),
        response));
    return response;
}

action::PduData fibonacci_feedback(
    const std::shared_ptr<action::ActionServerEndpointImpl>& action_server,
    std::vector<std::int32_t> partial_sequence)
{
    action::PduData packet;
    EXPECT_TRUE(action_server->create_feedback_buffer(packet));
    HakoCpp_FibonacciActionFeedback feedback{};
    feedback.body.partial_sequence = std::move(partial_sequence);
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
    const int encoded_size = convertor.cpp2pdu(
        feedback,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(encoded_size, 0);
    return packet;
}

action::PduData fibonacci_result(
    const std::shared_ptr<action::ActionServerEndpointImpl>& action_server,
    std::vector<std::int32_t> sequence)
{
    action::PduData packet;
    EXPECT_TRUE(action_server->create_result_buffer(packet));
    HakoCpp_FibonacciActionResponse result{};
    result.body.sequence = std::move(sequence);
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    const int encoded_size = convertor.cpp2pdu(
        result,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(encoded_size, 0);
    return packet;
}

HakoCpp_FibonacciActionFeedback receive_fibonacci_feedback(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& action_endpoint,
    HakoPduChannelIdType channel_id,
    std::size_t* received_size_out = nullptr)
{
    action::PduData packet(2048, 0);
    std::size_t received_size = 0;
    const hakoniwa::pdu::PduResolvedKey feedback_key{
        "fibonacci",
        channel_id,
    };
    EXPECT_EQ(
        action_endpoint->recv(
            feedback_key,
            std::as_writable_bytes(std::span(packet)),
            received_size),
        HAKO_PDU_ERR_OK);
    EXPECT_GT(received_size, 0U);
    if (received_size_out != nullptr) {
        *received_size_out = received_size;
    }
    packet.resize(received_size);

    HakoCpp_FibonacciActionFeedback feedback{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
    EXPECT_TRUE(convertor.pdu2cpp(
        reinterpret_cast<char*>(packet.data()), feedback));
    return feedback;
}

} // namespace

TEST(ActionServerInitializationContract, AcceptsResolvedLogicalConfiguration)
{
    action::ActionServerEndpointImpl action_server(
        "fibonacci", 1000, endpoint(), time_source());

    EXPECT_TRUE(action_server.initialize(fibonacci_action()));
}

TEST(ActionServerInitializationContract, CreatesPublicTypedEncodingBuffers)
{
    auto action_server = server();
    auto configuration = fibonacci_action();
    configuration["bufferHeap"] = {
        {"requestSize", 64},
        {"responseSize", 64},
        {"feedbackSize", 32},
    };

    action::PduData result_packet;
    action::PduData feedback_packet;
    EXPECT_FALSE(action_server->create_result_buffer(result_packet));
    EXPECT_FALSE(action_server->create_feedback_buffer(feedback_packet));
    ASSERT_TRUE(action_server->initialize(configuration));
    ASSERT_TRUE(action_server->create_result_buffer(result_packet));
    ASSERT_TRUE(action_server->create_feedback_buffer(feedback_packet));

    HakoPduMetaDataType result_metadata{};
    HakoPduMetaDataType feedback_metadata{};
    std::memcpy(&result_metadata, result_packet.data(), sizeof(result_metadata));
    std::memcpy(&feedback_metadata, feedback_packet.data(), sizeof(feedback_metadata));
    EXPECT_EQ(result_metadata.total_size, result_packet.size());
    EXPECT_EQ(result_metadata.total_size - result_metadata.heap_off, 64U);
    EXPECT_EQ(feedback_metadata.total_size, feedback_packet.size());
    EXPECT_EQ(feedback_metadata.total_size - feedback_metadata.heap_off, 32U);

    HakoCpp_FibonacciActionResponse result{};
    result.body.sequence = {0, 1, 1, 2, 3, 5};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse result_convertor;
    const int result_size = result_convertor.cpp2pdu(
        result,
        reinterpret_cast<char*>(result_packet.data()),
        static_cast<int>(result_packet.size()));
    ASSERT_GT(result_size, 0);
    EXPECT_LT(static_cast<std::size_t>(result_size), result_packet.size());

    HakoCpp_FibonacciActionFeedback feedback{};
    feedback.body.partial_sequence = {0, 1, 1, 2};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback feedback_convertor;
    const int feedback_size = feedback_convertor.cpp2pdu(
        feedback,
        reinterpret_cast<char*>(feedback_packet.data()),
        static_cast<int>(feedback_packet.size()));
    ASSERT_GT(feedback_size, 0);
    EXPECT_LT(static_cast<std::size_t>(feedback_size), feedback_packet.size());
}

TEST(ActionServerInitializationContract, RejectsMalformedConfiguration)
{
    auto action_server = server();
    auto configuration = fibonacci_action();
    configuration.erase("type");

    EXPECT_FALSE(action_server->initialize(configuration));
}

TEST(ActionServerInitializationContract, RejectsActionNameMismatch)
{
    auto action_server = std::make_shared<action::ActionServerEndpointImpl>(
        "move_robot", 1000, endpoint(), time_source());

    EXPECT_FALSE(action_server->initialize(fibonacci_action()));
}

TEST(ActionServerInitializationContract, RejectsActionTypeMissingFromRegistry)
{
    auto action_server = server();
    auto configuration = fibonacci_action();
    configuration["type"] = "missing_action_msgs/Missing";

    EXPECT_FALSE(action_server->initialize(configuration));
}

TEST(ActionServerInitializationContract, RequiresEndpointAndTimeSource)
{
    auto missing_endpoint = std::make_shared<action::ActionServerEndpointImpl>(
        "fibonacci", 1000, nullptr, time_source());
    auto missing_time_source = std::make_shared<action::ActionServerEndpointImpl>(
        "fibonacci", 1000, endpoint(), nullptr);

    EXPECT_FALSE(missing_endpoint->initialize(fibonacci_action()));
    EXPECT_FALSE(missing_time_source->initialize(fibonacci_action()));
}

TEST(ActionServerInitializationContract, RejectsRepeatedInitialization)
{
    auto action_server = server();

    ASSERT_TRUE(action_server->initialize(fibonacci_action()));
    EXPECT_FALSE(action_server->initialize(fibonacci_action()));
}

TEST(ActionServerInitializationContract, RoutesGoalRequestUsingResolvedChannel)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 3};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    EXPECT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(event.type, action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(event.action_name, "fibonacci");
    EXPECT_EQ(event.goal.goal_id, kGoalId);
    EXPECT_EQ(event.pdu, packet);

    EXPECT_EQ(
        action_server->poll(event),
        action::ServerEventType::NONE);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, IgnoresMalformedRequestPacket)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const action::PduData malformed{0x01, 0x02, 0x03};
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(malformed))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
    EXPECT_EQ(event.type, action::ServerEventType::NONE);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, RejectsRequestExceedingConfiguredHeap)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto configuration = fibonacci_action();
    configuration["bufferHeap"] = {
        {"requestSize", 0},
        {"responseSize", 1024},
        {"feedbackSize", 1024},
    };
    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(configuration));

    const auto packet = with_extra_heap(fibonacci_goal_request(), 1);
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
    EXPECT_EQ(event.type, action::ServerEventType::NONE);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, RejectsInvalidProtocolHeaders)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const action::GoalId zero_goal_id{};
    const std::vector<action::PduData> invalid_packets{
        fibonacci_goal_request(2, 1, kGoalId),
        fibonacci_goal_request(1, 99, kGoalId),
        fibonacci_goal_request(1, 1, zero_goal_id),
    };
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};

    for (const auto& packet : invalid_packets) {
        ASSERT_EQ(
            action_endpoint->send(
                request_key,
                std::as_bytes(std::span(packet))),
            HAKO_PDU_ERR_OK);

        action::ServerEvent event;
        EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
        EXPECT_EQ(event.type, action::ServerEventType::NONE);
    }

    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, ClearPendingEventsDropsRawPackets)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action_server->clear_pending_events();

    action::ServerEvent event;
    EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, AcceptGoalCommitsBindingOnce)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 6};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);

    EXPECT_TRUE(action_server->accept_goal(event.goal));
    EXPECT_FALSE(action_server->accept_goal(event.goal));

    std::size_t response_wire_size = 0;
    const auto response = receive_fibonacci_response(
        action_endpoint, 7, &response_wire_size);
    EXPECT_EQ(
        response_wire_size,
        static_cast<std::size_t>(HAKO_PDU_META_DATA_SIZE())
            + HAKO_ALIGN_SIZE(
                sizeof(Hako_FibonacciActionResponse),
                HAKO_ALIGNMENT_SIZE));
    EXPECT_EQ(response.header.version, 1);
    EXPECT_EQ(response.header.response_kind, 1);
    EXPECT_EQ(
        response.header.status,
        static_cast<std::uint8_t>(action::Decision::ACCEPTED));
    EXPECT_EQ(response.header.goal_id, kGoalId);
    EXPECT_TRUE(response.body.sequence.empty());

    const action::ServerGoalHandle unknown_goal{{
        0x50, 0x51, 0x52, 0x53,
        0x60, 0x61, 0x62, 0x63,
        0x70, 0x71, 0x72, 0x73,
        0x80, 0x81, 0x82, 0x83,
    }};
    const action::ServerGoalHandle invalid_goal{};
    EXPECT_FALSE(action_server->accept_goal(unknown_goal));
    EXPECT_FALSE(action_server->accept_goal(invalid_goal));
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, RejectGoalSendsRejectedResponse)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 3};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_TRUE(action_server->reject_goal(event.goal));
    EXPECT_FALSE(action_server->reject_goal(event.goal));
    EXPECT_FALSE(action_server->accept_goal(event.goal));

    const auto response = receive_fibonacci_response(action_endpoint, 4);
    EXPECT_EQ(response.header.version, 1);
    EXPECT_EQ(response.header.response_kind, 1);
    EXPECT_EQ(
        response.header.status,
        static_cast<std::uint8_t>(action::Decision::REJECTED));
    EXPECT_EQ(response.header.goal_id, kGoalId);
    EXPECT_TRUE(response.body.sequence.empty());
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, SendFailureDoesNotReopenGoalDecision)
{
    auto action_endpoint = std::make_shared<ResponseFailingEndpoint>();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);

    EXPECT_FALSE(action_server->accept_goal(event.goal));
    EXPECT_FALSE(action_server->accept_goal(event.goal));
    EXPECT_FALSE(action_server->reject_goal(event.goal));
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, SendsFeedbackFromZeroAfterGoalAccepted)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));
    const auto feedback = fibonacci_feedback(action_server, {0, 1, 1});

    const auto request = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(request))),
        HAKO_PDU_ERR_OK);
    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);

    EXPECT_FALSE(action_server->send_feedback(event.goal, feedback));
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    (void)receive_fibonacci_response(action_endpoint, 1);

    ASSERT_TRUE(action_server->send_feedback(event.goal, feedback));
    std::size_t first_wire_size = 0;
    const auto first = receive_fibonacci_feedback(
        action_endpoint, 2, &first_wire_size);
    EXPECT_EQ(first.header.version, 1);
    EXPECT_EQ(first.header.goal_id, kGoalId);
    EXPECT_EQ(first.header.sequence_no, 0U);
    EXPECT_EQ(first.body.partial_sequence, (std::vector<std::int32_t>{0, 1, 1}));
    EXPECT_LT(first_wire_size, feedback.size());

    const auto second_packet = fibonacci_feedback(
        action_server, {0, 1, 1, 2});
    ASSERT_TRUE(action_server->send_feedback(event.goal, second_packet));
    const auto second = receive_fibonacci_feedback(action_endpoint, 2);
    EXPECT_EQ(second.header.sequence_no, 1U);
    EXPECT_EQ(
        second.body.partial_sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2}));
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, FeedbackSendFailureDoesNotAdvanceSequence)
{
    auto action_endpoint = std::make_shared<FeedbackFailingOnceEndpoint>();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));
    const auto request = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(request))),
        HAKO_PDU_ERR_OK);
    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    (void)receive_fibonacci_response(action_endpoint, 1);

    const auto feedback = fibonacci_feedback(action_server, {0, 1});
    EXPECT_FALSE(action_server->send_feedback(event.goal, feedback));
    ASSERT_TRUE(action_server->send_feedback(event.goal, feedback));
    const auto received = receive_fibonacci_feedback(action_endpoint, 2);
    EXPECT_EQ(received.header.sequence_no, 0U);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, FeedbackSequenceIsIndependentPerGoal)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));
    const auto second_id = action::GoalId{
        0x50, 0x51, 0x52, 0x53,
        0x60, 0x61, 0x62, 0x63,
        0x70, 0x71, 0x72, 0x73,
        0x80, 0x81, 0x82, 0x83,
    };
    const auto first_request = fibonacci_goal_request();
    const auto second_request = fibonacci_goal_request(1, 1, second_id);
    const hakoniwa::pdu::PduResolvedKey first_key{"fibonacci", 0};
    const hakoniwa::pdu::PduResolvedKey second_key{"fibonacci", 3};
    ASSERT_EQ(
        action_endpoint->send(
            first_key, std::as_bytes(std::span(first_request))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_endpoint->send(
            second_key, std::as_bytes(std::span(second_request))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent first_event;
    action::ServerEvent second_event;
    ASSERT_EQ(
        action_server->poll(first_event),
        action::ServerEventType::GOAL_REQUEST);
    ASSERT_EQ(
        action_server->poll(second_event),
        action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(action_server->accept_goal(first_event.goal));
    ASSERT_TRUE(action_server->accept_goal(second_event.goal));
    (void)receive_fibonacci_response(action_endpoint, 1);
    (void)receive_fibonacci_response(action_endpoint, 4);

    const auto feedback = fibonacci_feedback(action_server, {0, 1});
    ASSERT_TRUE(action_server->send_feedback(first_event.goal, feedback));
    ASSERT_TRUE(action_server->send_feedback(second_event.goal, feedback));
    const auto first = receive_fibonacci_feedback(action_endpoint, 2);
    const auto second = receive_fibonacci_feedback(action_endpoint, 5);
    EXPECT_EQ(first.header.goal_id, kGoalId);
    EXPECT_EQ(first.header.sequence_no, 0U);
    EXPECT_EQ(second.header.goal_id, second_id);
    EXPECT_EQ(second.header.sequence_no, 0U);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, CompletesGoalAndReleasesSlotAfterResultSend)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));
    const auto request = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(request))),
        HAKO_PDU_ERR_OK);
    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);

    const auto result = fibonacci_result(
        action_server, {0, 1, 1, 2, 3, 5});
    EXPECT_FALSE(action_server->complete(
        event.goal, action::TerminalStatus::SUCCEEDED, result));
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    (void)receive_fibonacci_response(action_endpoint, 1);
    EXPECT_FALSE(action_server->complete(
        event.goal, action::TerminalStatus::SUCCEEDED, {}));
    ASSERT_TRUE(action_server->complete(
        event.goal, action::TerminalStatus::SUCCEEDED, result));
    EXPECT_FALSE(action_server->complete(
        event.goal, action::TerminalStatus::SUCCEEDED, result));
    EXPECT_FALSE(action_server->send_feedback(
        event.goal, fibonacci_feedback(action_server, {0, 1})));

    std::size_t result_wire_size = 0;
    const auto response = receive_fibonacci_response(
        action_endpoint, 1, &result_wire_size);
    EXPECT_EQ(response.header.version, 1);
    EXPECT_EQ(response.header.response_kind, 3);
    EXPECT_EQ(
        response.header.status,
        static_cast<std::uint8_t>(action::TerminalStatus::SUCCEEDED));
    EXPECT_EQ(response.header.goal_id, kGoalId);
    EXPECT_EQ(
        response.body.sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2, 3, 5}));
    EXPECT_LT(result_wire_size, result.size());

    const auto second_id = action::GoalId{
        0x50, 0x51, 0x52, 0x53,
        0x60, 0x61, 0x62, 0x63,
        0x70, 0x71, 0x72, 0x73,
        0x80, 0x81, 0x82, 0x83,
    };
    const auto second_request = fibonacci_goal_request(1, 1, second_id);
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(second_request))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(event.goal.goal_id, second_id);
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    (void)receive_fibonacci_response(action_endpoint, 1);
    EXPECT_FALSE(action_server->complete(
        event.goal, action::TerminalStatus::CANCELED, result));
    EXPECT_TRUE(action_server->complete(
        event.goal, action::TerminalStatus::ABORTED, result));
    const auto aborted = receive_fibonacci_response(action_endpoint, 1);
    EXPECT_EQ(
        aborted.header.status,
        static_cast<std::uint8_t>(action::TerminalStatus::ABORTED));
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, ResultSendFailureRetainsFinishingSlot)
{
    auto action_endpoint = std::make_shared<ResultFailingOnceEndpoint>();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));
    const auto request = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(request))),
        HAKO_PDU_ERR_OK);
    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    (void)receive_fibonacci_response(action_endpoint, 1);

    const auto result = fibonacci_result(action_server, {0, 1, 1});
    EXPECT_FALSE(action_server->complete(
        event.goal, action::TerminalStatus::SUCCEEDED, result));
    EXPECT_FALSE(action_server->complete(
        event.goal, action::TerminalStatus::SUCCEEDED, result));
    EXPECT_FALSE(action_server->send_feedback(
        event.goal, fibonacci_feedback(action_server, {0, 1})));

    const auto second_id = action::GoalId{
        0x50, 0x51, 0x52, 0x53,
        0x60, 0x61, 0x62, 0x63,
        0x70, 0x71, 0x72, 0x73,
        0x80, 0x81, 0x82, 0x83,
    };
    const auto second_request = fibonacci_goal_request(1, 1, second_id);
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(second_request))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
    const auto rejected = receive_fibonacci_response(action_endpoint, 1);
    EXPECT_EQ(rejected.header.goal_id, second_id);
    EXPECT_EQ(
        rejected.header.status,
        static_cast<std::uint8_t>(action::Decision::REJECTED));
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, DuplicateGoalIsNotDispatchedAgain)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey first_slot{"fibonacci", 0};
    const hakoniwa::pdu::PduResolvedKey second_slot{"fibonacci", 3};
    ASSERT_EQ(
        action_endpoint->send(
            first_slot,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_endpoint->send(
            second_slot,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    EXPECT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, RejectsDifferentGoalOnOwnedSlot)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    const auto first_packet = fibonacci_goal_request();
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(first_packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    const auto accepted = receive_fibonacci_response(action_endpoint, 1);
    EXPECT_EQ(accepted.header.goal_id, kGoalId);
    EXPECT_EQ(
        accepted.header.status,
        static_cast<std::uint8_t>(action::Decision::ACCEPTED));

    const auto second_id = action::GoalId{
        0x50, 0x51, 0x52, 0x53,
        0x60, 0x61, 0x62, 0x63,
        0x70, 0x71, 0x72, 0x73,
        0x80, 0x81, 0x82, 0x83,
    };
    const auto second_packet = fibonacci_goal_request(1, 1, second_id);
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(second_packet))),
        HAKO_PDU_ERR_OK);

    EXPECT_EQ(action_server->poll(event), action::ServerEventType::NONE);
    const auto rejected = receive_fibonacci_response(action_endpoint, 1);
    EXPECT_EQ(rejected.header.goal_id, second_id);
    EXPECT_EQ(
        rejected.header.status,
        static_cast<std::uint8_t>(action::Decision::REJECTED));

    action_server->reset_contexts();
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(second_packet))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(event.goal.goal_id, second_id);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, ClearDoesNotInvalidateDispatchedGoalDecision)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key,
            std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    action_server->clear_pending_events();

    EXPECT_TRUE(action_server->accept_goal(event.goal));
    const auto response = receive_fibonacci_response(action_endpoint, 1);
    EXPECT_EQ(response.header.goal_id, kGoalId);
    EXPECT_EQ(
        response.header.status,
        static_cast<std::uint8_t>(action::Decision::ACCEPTED));
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionServerInitializationContract, ResetInvalidatesContextsAndReleasesSlots)
{
    auto action_endpoint = endpoint();
    ASSERT_EQ(
        action_endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(action_endpoint->start(), HAKO_PDU_ERR_OK);

    auto action_server = server(action_endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto packet = fibonacci_goal_request();
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    action_server->reset_contexts();

    EXPECT_FALSE(action_server->accept_goal(event.goal));
    ASSERT_EQ(
        action_endpoint->send(
            request_key, std::as_bytes(std::span(packet))),
        HAKO_PDU_ERR_OK);
    EXPECT_EQ(
        action_server->poll(event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(action_endpoint->stop(), HAKO_PDU_ERR_OK);
}
