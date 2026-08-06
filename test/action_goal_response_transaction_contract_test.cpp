#include "action_server_endpoint_impl.hpp"

#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/virtual_time_source.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace action = hakoniwa::pdu::action;

namespace {

class GoalResponseBlockingEndpoint final : public hakoniwa::pdu::Endpoint {
public:
    GoalResponseBlockingEndpoint()
        : Endpoint("fibonacci-server", HAKO_PDU_ENDPOINT_DIRECTION_INOUT)
    {
    }

    HakoPduErrorType send(
        const hakoniwa::pdu::PduResolvedKey& key,
        std::span<const std::byte> data) noexcept override
    {
        if (key.channel_id % 3 == 1) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (++response_send_count_ == 1) {
                response_blocked_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this] { return release_response_; });
            }
        }
        return Endpoint::send(key, data);
    }

    bool wait_until_response_blocked()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this] { return response_blocked_; });
    }

    void release_response()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release_response_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t response_send_count_{0};
    bool response_blocked_{false};
    bool release_response_{false};
};

class GoalResponseFailingOnceEndpoint final : public hakoniwa::pdu::Endpoint {
public:
    GoalResponseFailingOnceEndpoint()
        : Endpoint("fibonacci-server", HAKO_PDU_ENDPOINT_DIRECTION_INOUT)
    {
    }

    HakoPduErrorType send(
        const hakoniwa::pdu::PduResolvedKey& key,
        std::span<const std::byte> data) noexcept override
    {
        if (key.channel_id % 3 == 1 && ++response_send_count_ == 1) {
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

std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source()
{
    return std::make_shared<hakoniwa::time_source::VirtualTimeSource>();
}

std::shared_ptr<action::ActionServerEndpointImpl> server(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& endpoint)
{
    return std::make_shared<action::ActionServerEndpointImpl>(
        "fibonacci", 1000, endpoint, time_source());
}

action::GoalId goal_id(std::uint8_t seed)
{
    action::GoalId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::uint8_t>(seed + index);
    }
    return id;
}

action::PduData request_packet(
    const action::GoalId& id,
    std::uint8_t request_kind)
{
    HakoCpp_FibonacciActionRequest request{};
    request.header.version = 1;
    request.header.request_kind = request_kind;
    request.header.goal_id = id;
    request.body.order = request_kind == 1 ? 8 : 0;

    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
    action::PduData packet(1024, 0);
    const int size = convertor.cpp2pdu(
        request,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    EXPECT_GT(size, 0);
    if (size > 0) {
        packet.resize(static_cast<std::size_t>(size));
    }
    return packet;
}

action::PduData goal_request(const action::GoalId& id)
{
    return request_packet(id, 1);
}

action::PduData cancel_request(const action::GoalId& id)
{
    return request_packet(id, 2);
}

action::PduData feedback_packet(
    const std::shared_ptr<action::ActionServerEndpointImpl>& action_server)
{
    action::PduData packet;
    EXPECT_TRUE(action_server->create_feedback_buffer(packet));
    HakoCpp_FibonacciActionFeedback feedback{};
    feedback.body.partial_sequence = {0, 1};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
    EXPECT_GT(
        convertor.cpp2pdu(
            feedback,
            reinterpret_cast<char*>(packet.data()),
            static_cast<int>(packet.size())),
        0);
    return packet;
}

action::PduData result_packet(
    const std::shared_ptr<action::ActionServerEndpointImpl>& action_server)
{
    action::PduData packet;
    EXPECT_TRUE(action_server->create_result_buffer(packet));
    HakoCpp_FibonacciActionResponse response{};
    response.body.sequence = {0, 1, 1};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    EXPECT_GT(
        convertor.cpp2pdu(
            response,
            reinterpret_cast<char*>(packet.data()),
            static_cast<int>(packet.size())),
        0);
    return packet;
}

HakoCpp_FibonacciActionResponse receive_response(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& endpoint,
    HakoPduChannelIdType channel_id = 1)
{
    action::PduData packet(1024, 0);
    std::size_t size = 0;
    EXPECT_EQ(
        endpoint->recv(
            hakoniwa::pdu::PduResolvedKey{"fibonacci", channel_id},
            std::as_writable_bytes(std::span(packet)),
            size),
        HAKO_PDU_ERR_OK);
    EXPECT_GT(size, 0U);
    packet.resize(size);

    HakoCpp_FibonacciActionResponse response{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    EXPECT_TRUE(convertor.pdu2cpp(
        reinterpret_cast<char*>(packet.data()), response));
    return response;
}

action::ServerEvent dispatch_goal(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& endpoint,
    const std::shared_ptr<action::ActionServerEndpointImpl>& action_server,
    const action::GoalId& id)
{
    const auto request = goal_request(id);
    EXPECT_EQ(
        endpoint->send(
            hakoniwa::pdu::PduResolvedKey{"fibonacci", 0},
            std::as_bytes(std::span(request))),
        HAKO_PDU_ERR_OK);
    action::ServerEvent event;
    EXPECT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);
    return event;
}

} // namespace

TEST(ActionGoalResponseTransactionContract, AcceptResponsePrecedesResult)
{
    auto endpoint = std::make_shared<GoalResponseBlockingEndpoint>();
    ASSERT_EQ(endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH), HAKO_PDU_ERR_OK);
    ASSERT_EQ(endpoint->start(), HAKO_PDU_ERR_OK);
    auto action_server = server(endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto event = dispatch_goal(endpoint, action_server, goal_id(0x10));
    const auto result = result_packet(action_server);

    bool accepted = false;
    bool result_completed = false;
    std::thread accept_thread([&] {
        accepted = action_server->accept_goal(event.goal);
    });

    if (!endpoint->wait_until_response_blocked()) {
        endpoint->release_response();
        accept_thread.join();
        FAIL() << "Goal Response send did not enter the blocked state.";
    }

    std::thread complete_thread([&] {
        result_completed = action_server->complete(
            event.goal, action::TerminalStatus::SUCCEEDED, result);
    });

    endpoint->release_response();
    accept_thread.join();
    complete_thread.join();
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(result_completed);

    const auto accepted_response = receive_response(endpoint);
    EXPECT_EQ(accepted_response.header.response_kind, 1);
    EXPECT_EQ(
        accepted_response.header.status,
        static_cast<std::uint8_t>(action::Decision::ACCEPTED));

    const auto completed = receive_response(endpoint);
    EXPECT_EQ(completed.header.response_kind, 3);
    EXPECT_EQ(endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionGoalResponseTransactionContract, QueuesCancelUntilAcceptResponseCompletes)
{
    auto endpoint = std::make_shared<GoalResponseBlockingEndpoint>();
    ASSERT_EQ(endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH), HAKO_PDU_ERR_OK);
    ASSERT_EQ(endpoint->start(), HAKO_PDU_ERR_OK);
    auto action_server = server(endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto id = goal_id(0x20);
    const auto event = dispatch_goal(endpoint, action_server, id);
    bool accepted = false;
    std::thread accept_thread([&] {
        accepted = action_server->accept_goal(event.goal);
    });

    ASSERT_TRUE(endpoint->wait_until_response_blocked());
    const auto cancel = cancel_request(id);
    ASSERT_EQ(
        endpoint->send(
            hakoniwa::pdu::PduResolvedKey{"fibonacci", 0},
            std::as_bytes(std::span(cancel))),
        HAKO_PDU_ERR_OK);

    endpoint->release_response();
    accept_thread.join();
    ASSERT_TRUE(accepted);
    (void)receive_response(endpoint);

    action::ServerEvent deferred;
    EXPECT_EQ(
        action_server->poll(deferred), action::ServerEventType::CANCEL_REQUEST);
    EXPECT_EQ(deferred.goal.goal_id, id);
    EXPECT_EQ(endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionGoalResponseTransactionContract, QueuesNextGoalUntilRejectResponseReleasesSlot)
{
    auto endpoint = std::make_shared<GoalResponseBlockingEndpoint>();
    ASSERT_EQ(endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH), HAKO_PDU_ERR_OK);
    ASSERT_EQ(endpoint->start(), HAKO_PDU_ERR_OK);
    auto action_server = server(endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto first = dispatch_goal(endpoint, action_server, goal_id(0x28));
    bool rejected = false;
    std::thread reject_thread([&] {
        rejected = action_server->reject_goal(first.goal);
    });

    ASSERT_TRUE(endpoint->wait_until_response_blocked());
    const auto next_id = goal_id(0x48);
    const auto next_request = goal_request(next_id);
    ASSERT_EQ(
        endpoint->send(
            hakoniwa::pdu::PduResolvedKey{"fibonacci", 0},
            std::as_bytes(std::span(next_request))),
        HAKO_PDU_ERR_OK);

    endpoint->release_response();
    reject_thread.join();
    ASSERT_TRUE(rejected);
    (void)receive_response(endpoint);

    action::ServerEvent deferred;
    EXPECT_EQ(
        action_server->poll(deferred), action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(deferred.goal.goal_id, next_id);
    EXPECT_EQ(endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionGoalResponseTransactionContract, RetriesAcceptedGoalResponseAfterFailure)
{
    auto endpoint = std::make_shared<GoalResponseFailingOnceEndpoint>();
    ASSERT_EQ(endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH), HAKO_PDU_ERR_OK);
    ASSERT_EQ(endpoint->start(), HAKO_PDU_ERR_OK);
    auto action_server = server(endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto event = dispatch_goal(endpoint, action_server, goal_id(0x30));
    EXPECT_FALSE(action_server->accept_goal(event.goal));
    EXPECT_TRUE(action_server->accept_goal(event.goal));

    const auto response = receive_response(endpoint);
    EXPECT_EQ(response.header.response_kind, 1);
    EXPECT_EQ(
        response.header.status,
        static_cast<std::uint8_t>(action::Decision::ACCEPTED));
    EXPECT_EQ(endpoint->stop(), HAKO_PDU_ERR_OK);
}

TEST(ActionGoalResponseTransactionContract, RetriesRejectedGoalResponseAndReleasesSlot)
{
    auto endpoint = std::make_shared<GoalResponseFailingOnceEndpoint>();
    ASSERT_EQ(endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH), HAKO_PDU_ERR_OK);
    ASSERT_EQ(endpoint->start(), HAKO_PDU_ERR_OK);
    auto action_server = server(endpoint);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto event = dispatch_goal(endpoint, action_server, goal_id(0x50));
    EXPECT_FALSE(action_server->reject_goal(event.goal));
    EXPECT_TRUE(action_server->reject_goal(event.goal));

    const auto rejected = receive_response(endpoint);
    EXPECT_EQ(rejected.header.response_kind, 1);
    EXPECT_EQ(
        rejected.header.status,
        static_cast<std::uint8_t>(action::Decision::REJECTED));

    const auto next = dispatch_goal(endpoint, action_server, goal_id(0x70));
    EXPECT_EQ(next.goal.goal_id, goal_id(0x70));
    EXPECT_EQ(endpoint->stop(), HAKO_PDU_ERR_OK);
}
