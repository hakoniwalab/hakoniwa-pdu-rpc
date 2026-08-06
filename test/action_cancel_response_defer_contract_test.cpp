#include "action_server_endpoint_impl.hpp"

#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/virtual_time_source.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
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

namespace action = hakoniwa::pdu::action;

namespace {

class CancelResponseBlockingEndpoint final : public hakoniwa::pdu::Endpoint {
public:
    CancelResponseBlockingEndpoint()
        : Endpoint("fibonacci-server", HAKO_PDU_ENDPOINT_DIRECTION_INOUT)
    {
    }

    HakoPduErrorType send(
        const hakoniwa::pdu::PduResolvedKey& key,
        std::span<const std::byte> data) noexcept override
    {
        if (key.channel_id % 3 == 1) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (++response_send_count_ == 2) {
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

HakoCpp_FibonacciActionResponse receive_response(
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& endpoint)
{
    action::PduData packet(1024, 0);
    std::size_t size = 0;
    EXPECT_EQ(
        endpoint->recv(
            hakoniwa::pdu::PduResolvedKey{"fibonacci", 1},
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

} // namespace

TEST(ActionCancelResponseDeferContract, DefersRepeatedCancelUntilRejectResponseCompletes)
{
    auto endpoint = std::make_shared<CancelResponseBlockingEndpoint>();
    ASSERT_EQ(endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH), HAKO_PDU_ERR_OK);
    ASSERT_EQ(endpoint->start(), HAKO_PDU_ERR_OK);

    auto time_source =
        std::make_shared<hakoniwa::time_source::VirtualTimeSource>();
    auto action_server = std::make_shared<action::ActionServerEndpointImpl>(
        "fibonacci", 1000, endpoint, time_source);
    ASSERT_TRUE(action_server->initialize(fibonacci_action()));

    const auto id = goal_id(0x40);
    const hakoniwa::pdu::PduResolvedKey request_key{"fibonacci", 0};
    const auto goal = request_packet(id, 1);
    ASSERT_EQ(
        endpoint->send(request_key, std::as_bytes(std::span(goal))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent event;
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(action_server->accept_goal(event.goal));
    (void)receive_response(endpoint);

    const auto cancel = request_packet(id, 2);
    ASSERT_EQ(
        endpoint->send(request_key, std::as_bytes(std::span(cancel))),
        HAKO_PDU_ERR_OK);
    ASSERT_EQ(
        action_server->poll(event), action::ServerEventType::CANCEL_REQUEST);
    const auto cancel_goal = event.goal;

    bool rejected = false;
    std::thread reject_thread([&] {
        rejected = action_server->reject_cancel(cancel_goal);
    });

    ASSERT_TRUE(endpoint->wait_until_response_blocked());
    ASSERT_EQ(
        endpoint->send(request_key, std::as_bytes(std::span(cancel))),
        HAKO_PDU_ERR_OK);

    action::ServerEvent deferred;
    EXPECT_EQ(action_server->poll(deferred), action::ServerEventType::NONE);

    endpoint->release_response();
    reject_thread.join();
    ASSERT_TRUE(rejected);
    const auto response = receive_response(endpoint);
    EXPECT_EQ(response.header.response_kind, 2);
    EXPECT_EQ(
        response.header.status,
        static_cast<std::uint8_t>(action::Decision::REJECTED));

    EXPECT_EQ(
        action_server->poll(deferred), action::ServerEventType::CANCEL_REQUEST);
    EXPECT_EQ(deferred.goal.goal_id, id);
    EXPECT_EQ(endpoint->stop(), HAKO_PDU_ERR_OK);
}
