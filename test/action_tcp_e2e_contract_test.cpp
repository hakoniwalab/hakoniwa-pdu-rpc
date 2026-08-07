#include <gtest/gtest.h>

#include "hakoniwa/pdu/action/action_services_client.hpp"
#include "hakoniwa/pdu/action/action_services_server.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace action = hakoniwa::pdu::action;
using namespace std::chrono_literals;

constexpr const char* kActionName = "fibonacci";
constexpr const char* kClientNodeId = "fibonacci-client";
constexpr const char* kServerNodeId = "fibonacci-server";
constexpr const char* kClientName = "action-tcp-e2e-client";

action::GoalId goal_id(std::uint8_t seed)
{
    action::GoalId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::uint8_t>(seed + index);
    }
    return id;
}

class ActionTcpRuntime {
public:
    ActionTcpRuntime()
        : server_container_(
              std::make_shared<hakoniwa::pdu::EndpointContainer>(
                  kServerNodeId, ACTION_TCP_ENDPOINTS_FIXTURE_PATH))
        , client_container_(
              std::make_shared<hakoniwa::pdu::EndpointContainer>(
                  kClientNodeId, ACTION_TCP_ENDPOINTS_FIXTURE_PATH))
        , server_(kServerNodeId, ACTION_TCP_CONFIG_FIXTURE_PATH)
        , client_(
              kClientNodeId,
              kClientName,
              ACTION_TCP_CONFIG_FIXTURE_PATH)
    {
    }

    ~ActionTcpRuntime()
    {
        stop();
    }

    bool start()
    {
        if (server_container_->initialize() != HAKO_PDU_ERR_OK
            || client_container_->initialize() != HAKO_PDU_ERR_OK) {
            return false;
        }
        initialized_ = true;

        if (!server_.initialize_services(server_container_)
            || !client_.initialize_services(client_container_)) {
            return false;
        }
        if (server_container_->start_all() != HAKO_PDU_ERR_OK
            || client_container_->start_all() != HAKO_PDU_ERR_OK) {
            return false;
        }
        if (!server_.start_all_services() || !client_.start_all_services()) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!server_container_->is_running_all()
               || !client_container_->is_running_all()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    void stop()
    {
        if (!initialized_) {
            return;
        }
        server_container_->stop_all();
        client_container_->stop_all();
        server_.stop_all_services();
        client_.stop_all_services();
        server_.clear_all_instances();
        client_.clear_all_instances();
        initialized_ = false;
    }

    action::ServerEventType wait_server_event(
        std::string& action_name,
        action::ServerEvent& event,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto type = server_.poll(action_name, event);
            if (type != action::ServerEventType::NONE) {
                return type;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return action::ServerEventType::NONE;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    action::ClientEventType wait_client_event(
        std::string& action_name,
        action::ClientEvent& event,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto type = client_.poll(action_name, event);
            if (type != action::ClientEventType::NONE) {
                return type;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return action::ClientEventType::NONE;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    action::PduData encode_goal(std::int32_t order)
    {
        action::PduData packet;
        if (!client_.create_goal_buffer(kActionName, packet)) {
            return {};
        }
        HakoCpp_FibonacciActionRequest request{};
        request.body.order = order;
        hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
        if (convertor.cpp2pdu(
                request,
                reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size())) <= 0) {
            return {};
        }
        return packet;
    }

    action::PduData encode_feedback(std::vector<std::int32_t> values)
    {
        action::PduData packet;
        if (!server_.create_feedback_buffer(kActionName, packet)) {
            return {};
        }
        HakoCpp_FibonacciActionFeedback feedback{};
        feedback.body.partial_sequence = std::move(values);
        hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
        if (convertor.cpp2pdu(
                feedback,
                reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size())) <= 0) {
            return {};
        }
        return packet;
    }

    action::PduData encode_result(std::vector<std::int32_t> values)
    {
        action::PduData packet;
        if (!server_.create_result_buffer(kActionName, packet)) {
            return {};
        }
        HakoCpp_FibonacciActionResponse response{};
        response.body.sequence = std::move(values);
        hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
        if (convertor.cpp2pdu(
                response,
                reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size())) <= 0) {
            return {};
        }
        return packet;
    }

    action::ActionServicesServer& server() { return server_; }
    action::ActionServicesClient& client() { return client_; }

private:
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> server_container_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> client_container_;
    action::ActionServicesServer server_;
    action::ActionServicesClient client_;
    bool initialized_{false};
};

TEST(ActionTcpE2EContract, GoalFeedbackAndResultRoundTrip)
{
    ActionTcpRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0x10);
    auto goal_packet = runtime.encode_goal(7);
    ASSERT_FALSE(goal_packet.empty());
    action::ClientGoalHandle client_goal;
    ASSERT_TRUE(runtime.client().send_goal(
        kActionName, goal_packet, id, client_goal));

    std::string action_name;
    action::ServerEvent server_event;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, server_event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(action_name, kActionName);
    EXPECT_EQ(server_event.goal.goal_id, id);
    HakoCpp_FibonacciActionRequest decoded_goal{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest goal_convertor;
    ASSERT_TRUE(goal_convertor.pdu2cpp(
        reinterpret_cast<char*>(server_event.pdu.data()), decoded_goal));
    EXPECT_EQ(decoded_goal.body.order, 7);

    ASSERT_TRUE(runtime.server().accept_goal(
        kActionName, server_event.goal));
    action::ClientEvent client_event;
    ASSERT_EQ(
        runtime.wait_client_event(action_name, client_event),
        action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(client_event.decision, action::Decision::ACCEPTED);
    EXPECT_EQ(client_event.goal.goal_id, id);

    auto feedback_packet = runtime.encode_feedback({0, 1, 1, 2, 3});
    ASSERT_FALSE(feedback_packet.empty());
    ASSERT_TRUE(runtime.server().send_feedback(
        kActionName, server_event.goal, feedback_packet));
    ASSERT_EQ(
        runtime.wait_client_event(action_name, client_event),
        action::ClientEventType::FEEDBACK);
    HakoCpp_FibonacciActionFeedback decoded_feedback{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback feedback_convertor;
    ASSERT_TRUE(feedback_convertor.pdu2cpp(
        reinterpret_cast<char*>(client_event.pdu.data()), decoded_feedback));
    EXPECT_EQ(
        decoded_feedback.body.partial_sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2, 3}));

    auto result_packet = runtime.encode_result({0, 1, 1, 2, 3, 5, 8});
    ASSERT_FALSE(result_packet.empty());
    ASSERT_TRUE(runtime.server().complete(
        kActionName,
        server_event.goal,
        action::TerminalStatus::SUCCEEDED,
        result_packet));
    ASSERT_EQ(
        runtime.wait_client_event(action_name, client_event),
        action::ClientEventType::RESULT);
    EXPECT_EQ(
        client_event.terminal_status,
        action::TerminalStatus::SUCCEEDED);
    HakoCpp_FibonacciActionResponse decoded_result{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse result_convertor;
    ASSERT_TRUE(result_convertor.pdu2cpp(
        reinterpret_cast<char*>(client_event.pdu.data()), decoded_result));
    EXPECT_EQ(
        decoded_result.body.sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2, 3, 5, 8}));
}

TEST(ActionTcpE2EContract, AcceptedCancelEndsWithCanceledResult)
{
    ActionTcpRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0x40);
    auto goal_packet = runtime.encode_goal(20);
    ASSERT_FALSE(goal_packet.empty());
    action::ClientGoalHandle client_goal;
    ASSERT_TRUE(runtime.client().send_goal(
        kActionName, goal_packet, id, client_goal));

    std::string action_name;
    action::ServerEvent server_event;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, server_event),
        action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(runtime.server().accept_goal(
        kActionName, server_event.goal));

    action::ClientEvent client_event;
    ASSERT_EQ(
        runtime.wait_client_event(action_name, client_event),
        action::ClientEventType::GOAL_RESPONSE);
    ASSERT_EQ(client_event.decision, action::Decision::ACCEPTED);

    ASSERT_TRUE(runtime.client().send_cancel(kActionName, client_goal));
    ASSERT_EQ(
        runtime.wait_server_event(action_name, server_event),
        action::ServerEventType::CANCEL_REQUEST);
    EXPECT_EQ(server_event.goal.goal_id, id);
    ASSERT_TRUE(runtime.server().accept_cancel(
        kActionName, server_event.goal));

    ASSERT_EQ(
        runtime.wait_client_event(action_name, client_event),
        action::ClientEventType::CANCEL_RESPONSE);
    EXPECT_EQ(client_event.decision, action::Decision::ACCEPTED);

    auto result_packet = runtime.encode_result({0, 1, 1, 2, 3});
    ASSERT_FALSE(result_packet.empty());
    ASSERT_TRUE(runtime.server().complete(
        kActionName,
        server_event.goal,
        action::TerminalStatus::CANCELED,
        result_packet));
    ASSERT_EQ(
        runtime.wait_client_event(action_name, client_event),
        action::ClientEventType::RESULT);
    EXPECT_EQ(
        client_event.terminal_status,
        action::TerminalStatus::CANCELED);
}

} // namespace
