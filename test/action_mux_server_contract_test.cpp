#include <gtest/gtest.h>

#include "hakoniwa/pdu/action/action_services_client.hpp"
#include "hakoniwa/pdu/action/action_services_mux_server.hpp"
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

action::GoalId goal_id(std::uint8_t seed)
{
    action::GoalId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::uint8_t>(seed + index);
    }
    return id;
}

class ActionMuxClient {
public:
    explicit ActionMuxClient(std::string client_name)
        : container_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              kClientNodeId,
              ACTION_MUX_CLIENT_ENDPOINTS_FIXTURE_PATH))
        , services_(
              kClientNodeId,
              std::move(client_name),
              ACTION_MUX_CONFIG_FIXTURE_PATH)
    {
    }

    bool start()
    {
        if (container_->initialize() != HAKO_PDU_ERR_OK
            || !services_.initialize_services(container_)
            || container_->start_all() != HAKO_PDU_ERR_OK
            || !services_.start_all_services()) {
            return false;
        }
        started_ = true;
        return true;
    }

    void stop()
    {
        if (!started_) {
            return;
        }
        (void)container_->stop_all();
        services_.stop_all_services();
        services_.clear_all_instances();
        started_ = false;
    }

    bool disconnect_transport()
    {
        return started_
            && container_->stop_all() == HAKO_PDU_ERR_OK;
    }

    action::PduData encode_goal(std::int32_t order)
    {
        action::PduData packet;
        if (!services_.create_goal_buffer(kActionName, packet)) {
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

    action::ClientEventType wait_event(
        action::ClientEvent& event,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string action_name;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto type = services_.poll(action_name, event);
            if (type != action::ClientEventType::NONE) {
                EXPECT_EQ(action_name, kActionName);
                return type;
            }
            std::this_thread::sleep_for(1ms);
        }
        return action::ClientEventType::NONE;
    }

    action::ActionServicesClient& services() { return services_; }

private:
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> container_;
    action::ActionServicesClient services_;
    bool started_{false};
};

class ActionMuxRuntime {
public:
    ActionMuxRuntime()
        : server_(
              kServerNodeId,
              ACTION_MUX_CONFIG_FIXTURE_PATH,
              ACTION_MUX_SERVER_ENDPOINT_FIXTURE_PATH)
        , client0_("action-mux-client-0")
        , client1_("action-mux-client-1")
    {
    }

    ~ActionMuxRuntime()
    {
        stop();
    }

    bool start()
    {
        if (!server_.initialize() || !server_.start()
            || !client0_.start() || !client1_.start()) {
            stop();
            return false;
        }
        started_ = true;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string ignored_name;
            action::ServerEvent ignored_event;
            (void)server_.poll(ignored_name, ignored_event);
            if (server_.is_ready()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void stop()
    {
        server_.stop();
        client0_.stop();
        client1_.stop();
        started_ = false;
    }

    action::ServerEventType wait_server_event(
        std::string& action_name,
        action::ServerEvent& event,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto type = server_.poll(action_name, event);
            if (type != action::ServerEventType::NONE) {
                return type;
            }
            std::this_thread::sleep_for(1ms);
        }
        return action::ServerEventType::NONE;
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

    action::ActionServicesMuxServer& server() { return server_; }
    ActionMuxClient& client0() { return client0_; }
    ActionMuxClient& client1() { return client1_; }

private:
    action::ActionServicesMuxServer server_;
    ActionMuxClient client0_;
    ActionMuxClient client1_;
    bool started_{false};
};

action::ServerEvent send_and_accept_goal(
    ActionMuxRuntime& runtime,
    ActionMuxClient& client,
    const action::GoalId& id,
    std::int32_t order)
{
    auto packet = client.encode_goal(order);
    EXPECT_FALSE(packet.empty());
    action::ClientGoalHandle handle;
    EXPECT_TRUE(client.services().send_goal(
        kActionName, packet, id, handle));

    std::string action_name;
    action::ServerEvent server_event;
    EXPECT_EQ(
        runtime.wait_server_event(action_name, server_event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(action_name, kActionName);
    EXPECT_EQ(server_event.goal.goal_id, id);
    EXPECT_TRUE(runtime.server().accept_goal(
        action_name, server_event.goal));

    action::ClientEvent client_event;
    EXPECT_EQ(
        client.wait_event(client_event),
        action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(client_event.decision, action::Decision::ACCEPTED);
    return server_event;
}

TEST(ActionMuxServerContract, RoutesTwoGoalsWithoutExposingConnectionIdentity)
{
    ActionMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());
    EXPECT_EQ(runtime.server().expected_count(), 2U);
    EXPECT_EQ(runtime.server().connected_count(), 2U);
    EXPECT_TRUE(runtime.server().is_ready());
    EXPECT_GE(
        runtime.server().connected_count(),
        runtime.server().expected_count());

    const auto goal0 = send_and_accept_goal(
        runtime, runtime.client0(), goal_id(0x10), 7);
    const auto goal1 = send_and_accept_goal(
        runtime, runtime.client1(), goal_id(0x30), 5);

    auto feedback = runtime.encode_feedback({0, 1, 1});
    ASSERT_FALSE(feedback.empty());
    ASSERT_TRUE(runtime.server().send_feedback(
        kActionName, goal0.goal, feedback));
    action::ClientEvent client_event;
    ASSERT_EQ(
        runtime.client0().wait_event(client_event),
        action::ClientEventType::FEEDBACK);
    EXPECT_EQ(client_event.goal.goal_id, goal0.goal.goal_id);

    auto result0 = runtime.encode_result({0, 1, 1, 2, 3, 5, 8});
    ASSERT_FALSE(result0.empty());
    ASSERT_TRUE(runtime.server().complete(
        kActionName,
        goal0.goal,
        action::TerminalStatus::SUCCEEDED,
        result0));
    ASSERT_EQ(
        runtime.client0().wait_event(client_event),
        action::ClientEventType::RESULT);
    EXPECT_EQ(client_event.goal.goal_id, goal0.goal.goal_id);

    action::ClientGoalHandle client_goal1{goal1.goal.goal_id};
    ASSERT_TRUE(runtime.client1().services().send_cancel(
        kActionName, client_goal1));
    std::string action_name;
    action::ServerEvent cancel_event;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, cancel_event),
        action::ServerEventType::CANCEL_REQUEST);
    EXPECT_EQ(cancel_event.goal.goal_id, goal1.goal.goal_id);
    ASSERT_TRUE(runtime.server().accept_cancel(
        action_name, cancel_event.goal));
    ASSERT_EQ(
        runtime.client1().wait_event(client_event),
        action::ClientEventType::CANCEL_RESPONSE);

    auto result1 = runtime.encode_result({0, 1, 1, 2, 3});
    ASSERT_FALSE(result1.empty());
    ASSERT_TRUE(runtime.server().complete(
        kActionName,
        goal1.goal,
        action::TerminalStatus::CANCELED,
        result1));
    ASSERT_EQ(
        runtime.client1().wait_event(client_event),
        action::ClientEventType::RESULT);
    EXPECT_EQ(client_event.terminal_status, action::TerminalStatus::CANCELED);
}

TEST(ActionMuxServerContract, RejectsDuplicateGoalIdAcrossConnections)
{
    ActionMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto duplicate_id = goal_id(0x60);
    auto packet0 = runtime.client0().encode_goal(8);
    auto packet1 = runtime.client1().encode_goal(13);
    ASSERT_FALSE(packet0.empty());
    ASSERT_FALSE(packet1.empty());
    action::ClientGoalHandle handle0;
    action::ClientGoalHandle handle1;
    ASSERT_TRUE(runtime.client0().services().send_goal(
        kActionName, packet0, duplicate_id, handle0));
    ASSERT_TRUE(runtime.client1().services().send_goal(
        kActionName, packet1, duplicate_id, handle1));

    std::string action_name;
    action::ServerEvent first_goal;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, first_goal),
        action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(runtime.server().accept_goal(action_name, first_goal.goal));

    action::ClientEvent event0;
    action::ClientEvent event1;
    auto type0 = action::ClientEventType::NONE;
    auto type1 = action::ClientEventType::NONE;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline
           && (type0 == action::ClientEventType::NONE
               || type1 == action::ClientEventType::NONE)) {
        // The duplicate packet may arrive after the first Goal was accepted.
        // Keep pumping the Mux so it can generate the protocol REJECT while
        // both Clients independently receive their Goal Responses.
        action::ServerEvent duplicate_event;
        const auto server_type = runtime.server().poll(
            action_name, duplicate_event);
        EXPECT_EQ(server_type, action::ServerEventType::NONE);

        std::string client_action;
        if (type0 == action::ClientEventType::NONE) {
            type0 = runtime.client0().services().poll(
                client_action, event0);
        }
        if (type1 == action::ClientEventType::NONE) {
            type1 = runtime.client1().services().poll(
                client_action, event1);
        }
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(type0, action::ClientEventType::GOAL_RESPONSE);
    ASSERT_EQ(type1, action::ClientEventType::GOAL_RESPONSE);
    EXPECT_NE(event0.decision, event1.decision);
    EXPECT_TRUE(
        (event0.decision == action::Decision::ACCEPTED
         && event1.decision == action::Decision::REJECTED)
        || (event0.decision == action::Decision::REJECTED
            && event1.decision == action::Decision::ACCEPTED));
}

TEST(ActionMuxServerContract, DiscardsUnacceptedGoalAfterDisconnect)
{
    ActionMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0x80);
    auto packet = runtime.client0().encode_goal(21);
    ASSERT_FALSE(packet.empty());
    action::ClientGoalHandle client_goal;
    ASSERT_TRUE(runtime.client0().services().send_goal(
        kActionName, packet, id, client_goal));

    std::string action_name;
    action::ServerEvent goal_event;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, goal_event),
        action::ServerEventType::GOAL_REQUEST);
    ASSERT_TRUE(runtime.client0().disconnect_transport());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline
           && runtime.server().connected_count() != 1) {
        action::ServerEvent ignored;
        (void)runtime.server().poll(action_name, ignored);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(runtime.server().connected_count(), 1U);
    EXPECT_FALSE(runtime.server().accept_goal(
        kActionName, goal_event.goal));
    EXPECT_FALSE(runtime.server().reject_goal(
        kActionName, goal_event.goal));
}

TEST(ActionMuxServerContract, RuntimeCancelCompletesDisconnectedGoalLocally)
{
    ActionMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0xA0);
    const auto accepted_goal = send_and_accept_goal(
        runtime, runtime.client0(), id, 34);
    ASSERT_EQ(accepted_goal.goal.goal_id, id);
    ASSERT_TRUE(runtime.client0().disconnect_transport());

    std::string action_name;
    action::ServerEvent runtime_cancel;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, runtime_cancel),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);
    EXPECT_EQ(action_name, kActionName);
    EXPECT_EQ(runtime_cancel.goal.goal_id, id);
    EXPECT_EQ(
        runtime_cancel.runtime_cancel_cause,
        action::RuntimeCancelCause::TRANSPORT_DISCONNECTED);
    ASSERT_TRUE(runtime.server().accept_cancel(
        action_name, runtime_cancel.goal));

    auto result = runtime.encode_result({0, 1, 1, 2, 3, 5});
    ASSERT_FALSE(result.empty());
    ASSERT_TRUE(runtime.server().complete(
        action_name,
        runtime_cancel.goal,
        action::TerminalStatus::CANCELED,
        result));
    EXPECT_EQ(runtime.server().connected_count(), 1U);

    // The disconnected Goal owner was released, so another connection may
    // reuse the same protocol identity.
    const auto reused_goal = send_and_accept_goal(
        runtime, runtime.client1(), id, 8);
    auto reused_result = runtime.encode_result({0, 1, 1, 2, 3, 5, 8});
    ASSERT_FALSE(reused_result.empty());
    ASSERT_TRUE(runtime.server().complete(
        kActionName,
        reused_goal.goal,
        action::TerminalStatus::SUCCEEDED,
        reused_result));
    action::ClientEvent client_event;
    EXPECT_EQ(
        runtime.client1().wait_event(client_event),
        action::ClientEventType::RESULT);
}

TEST(ActionMuxServerContract, DisconnectReclassifiesPendingClientCancel)
{
    ActionMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0xC0);
    const auto accepted_goal = send_and_accept_goal(
        runtime, runtime.client0(), id, 55);
    action::ClientGoalHandle client_goal{id};
    ASSERT_TRUE(runtime.client0().services().send_cancel(
        kActionName, client_goal));

    std::string action_name;
    action::ServerEvent client_cancel;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, client_cancel),
        action::ServerEventType::CANCEL_REQUEST);
    ASSERT_EQ(client_cancel.goal.goal_id, accepted_goal.goal.goal_id);
    ASSERT_TRUE(runtime.client0().disconnect_transport());

    action::ServerEvent runtime_cancel;
    ASSERT_EQ(
        runtime.wait_server_event(action_name, runtime_cancel),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);
    EXPECT_EQ(
        runtime_cancel.runtime_cancel_cause,
        action::RuntimeCancelCause::TRANSPORT_DISCONNECTED);
    ASSERT_TRUE(runtime.server().accept_cancel(
        action_name, runtime_cancel.goal));

    auto result = runtime.encode_result({0, 1, 1, 2, 3});
    ASSERT_FALSE(result.empty());
    EXPECT_TRUE(runtime.server().complete(
        action_name,
        runtime_cancel.goal,
        action::TerminalStatus::CANCELED,
        result));
}

} // namespace
