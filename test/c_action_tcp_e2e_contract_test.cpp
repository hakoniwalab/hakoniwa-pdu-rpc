#include <gtest/gtest.h>

#include "hakoniwa/pdu/action/c_action.h"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr const char* kActionName = "fibonacci";
constexpr const char* kClientNodeId = "fibonacci-client";
constexpr const char* kServerNodeId = "fibonacci-server";

struct OwnedBuffer {
    std::uint8_t* data{nullptr};
    std::size_t size{0};

    OwnedBuffer() = default;
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;
    OwnedBuffer(OwnedBuffer&& other) noexcept
        : data(std::exchange(other.data, nullptr))
        , size(std::exchange(other.size, 0))
    {
    }
    OwnedBuffer& operator=(OwnedBuffer&& other) noexcept
    {
        if (this != &other) {
            hako_pdu_action_buffer_free(data);
            data = std::exchange(other.data, nullptr);
            size = std::exchange(other.size, 0);
        }
        return *this;
    }
    ~OwnedBuffer() { hako_pdu_action_buffer_free(data); }
};

hako_pdu_action_goal_id_t goal_id(std::uint8_t seed)
{
    hako_pdu_action_goal_id_t id{};
    for (std::size_t index = 0; index < HAKO_PDU_ACTION_GOAL_ID_SIZE; ++index) {
        id.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return id;
}

bool same_goal(
    const hako_pdu_action_goal_id_t& lhs,
    const hako_pdu_action_goal_id_t& rhs)
{
    return std::memcmp(lhs.bytes, rhs.bytes, HAKO_PDU_ACTION_GOAL_ID_SIZE) == 0;
}

class CActionRuntime {
public:
    ~CActionRuntime() { stop(); }

    bool start()
    {
        server_ = hako_pdu_action_server_create(
            kServerNodeId,
            ACTION_C_TCP_CONFIG_FIXTURE_PATH,
            ACTION_C_TCP_ENDPOINTS_FIXTURE_PATH,
            1000,
            "real");
        client_ = hako_pdu_action_client_create(
            kClientNodeId,
            "c-action-e2e-client",
            ACTION_C_TCP_CONFIG_FIXTURE_PATH,
            ACTION_C_TCP_ENDPOINTS_FIXTURE_PATH,
            1000,
            "real");
        if (server_ == nullptr || client_ == nullptr) {
            return false;
        }
        if (hako_pdu_action_server_start(server_) != HAKO_PDU_ACTION_OK
            || hako_pdu_action_client_start(client_) != HAKO_PDU_ACTION_OK) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            int server_running = 0;
            int client_running = 0;
            if (hako_pdu_action_server_is_running(server_, &server_running)
                    != HAKO_PDU_ACTION_OK
                || hako_pdu_action_client_is_running(client_, &client_running)
                    != HAKO_PDU_ACTION_OK) {
                return false;
            }
            if (server_running != 0 && client_running != 0) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void stop()
    {
        if (client_ != nullptr) {
            hako_pdu_action_client_stop(client_);
            hako_pdu_action_client_destroy(client_);
            client_ = nullptr;
        }
        if (server_ != nullptr) {
            hako_pdu_action_server_stop(server_);
            hako_pdu_action_server_destroy(server_);
            server_ = nullptr;
        }
    }

    hako_pdu_action_error_t send_goal_result(
        std::int32_t order,
        const hako_pdu_action_goal_id_t& id,
        hako_pdu_action_client_goal_handle_t& goal_out)
    {
        OwnedBuffer packet;
        if (hako_pdu_action_client_create_goal_buffer_alloc(
                client_, kActionName, &packet.data, &packet.size)
            != HAKO_PDU_ACTION_OK) {
            return HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        HakoCpp_FibonacciActionRequest request{};
        request.body.order = order;
        hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
        if (convertor.cpp2pdu(
                request,
                reinterpret_cast<char*>(packet.data),
                static_cast<int>(packet.size)) <= 0) {
            return HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return hako_pdu_action_client_send_goal(
            client_,
            kActionName,
            packet.data,
            packet.size,
            &id,
            &goal_out,
            1'000'000);
    }

    bool send_goal(
        std::int32_t order,
        const hako_pdu_action_goal_id_t& id,
        hako_pdu_action_client_goal_handle_t& goal_out)
    {
        return send_goal_result(order, id, goal_out)
            == HAKO_PDU_ACTION_OK;
    }

    hako_pdu_action_server_event_t wait_server(
        hako_pdu_action_server_event_info_t& info,
        OwnedBuffer& packet,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            hako_pdu_action_error_t error = HAKO_PDU_ACTION_OK;
            const auto event = hako_pdu_action_server_poll_alloc(
                server_, &info, &packet.data, &packet.size, &error);
            if (event != HAKO_PDU_ACTION_SERVER_EVENT_NONE) {
                return event;
            }
            if (error != HAKO_PDU_ACTION_OK) {
                return HAKO_PDU_ACTION_SERVER_EVENT_ERROR;
            }
            std::this_thread::sleep_for(1ms);
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }

    hako_pdu_action_client_event_t wait_client(
        hako_pdu_action_client_event_info_t& info,
        OwnedBuffer& packet,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            hako_pdu_action_error_t error = HAKO_PDU_ACTION_OK;
            const auto event = hako_pdu_action_client_poll_alloc(
                client_, &info, &packet.data, &packet.size, &error);
            if (event != HAKO_PDU_ACTION_CLIENT_EVENT_NONE) {
                return event;
            }
            if (error != HAKO_PDU_ACTION_OK) {
                return HAKO_PDU_ACTION_CLIENT_EVENT_ERROR;
            }
            std::this_thread::sleep_for(1ms);
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }

    OwnedBuffer feedback(std::vector<std::int32_t> sequence)
    {
        OwnedBuffer packet;
        if (hako_pdu_action_server_create_feedback_buffer_alloc(
                server_, kActionName, &packet.data, &packet.size)
            != HAKO_PDU_ACTION_OK) {
            return {};
        }
        HakoCpp_FibonacciActionFeedback feedback{};
        feedback.body.partial_sequence = std::move(sequence);
        hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
        if (convertor.cpp2pdu(
                feedback,
                reinterpret_cast<char*>(packet.data),
                static_cast<int>(packet.size)) <= 0) {
            return {};
        }
        return packet;
    }

    OwnedBuffer result(std::vector<std::int32_t> sequence)
    {
        OwnedBuffer packet;
        if (hako_pdu_action_server_create_result_buffer_alloc(
                server_, kActionName, &packet.data, &packet.size)
            != HAKO_PDU_ACTION_OK) {
            return {};
        }
        HakoCpp_FibonacciActionResponse result{};
        result.body.sequence = std::move(sequence);
        hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
        if (convertor.cpp2pdu(
                result,
                reinterpret_cast<char*>(packet.data),
                static_cast<int>(packet.size)) <= 0) {
            return {};
        }
        return packet;
    }

    hako_pdu_action_client_handle_t* client() { return client_; }
    hako_pdu_action_server_handle_t* server() { return server_; }

private:
    hako_pdu_action_client_handle_t* client_{nullptr};
    hako_pdu_action_server_handle_t* server_{nullptr};
};

TEST(CActionTcpE2EContract, GoalFeedbackAndResult)
{
    CActionRuntime runtime;
    ASSERT_TRUE(runtime.start());

    std::size_t required_size = 0;
    EXPECT_EQ(
        hako_pdu_action_client_create_goal_buffer(
            runtime.client(), kActionName, nullptr, 0, &required_size),
        HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL);
    EXPECT_GT(required_size, 0U);
    required_size = 0;
    EXPECT_EQ(
        hako_pdu_action_server_create_feedback_buffer(
            runtime.server(), kActionName, nullptr, 0, &required_size),
        HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL);
    EXPECT_GT(required_size, 0U);
    required_size = 0;
    EXPECT_EQ(
        hako_pdu_action_server_create_result_buffer(
            runtime.server(), kActionName, nullptr, 0, &required_size),
        HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL);
    EXPECT_GT(required_size, 0U);

    const auto id = goal_id(0x10);
    hako_pdu_action_client_goal_handle_t client_goal{};
    ASSERT_TRUE(runtime.send_goal(8, id, client_goal));
    EXPECT_TRUE(same_goal(client_goal.goal_id, id));

    hako_pdu_action_server_event_info_t server_info{};
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        hako_pdu_action_error_t poll_error = HAKO_PDU_ACTION_OK;
        std::size_t event_size = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            EXPECT_EQ(
                hako_pdu_action_server_poll(
                    runtime.server(),
                    &server_info,
                    nullptr,
                    0,
                    &event_size,
                    &poll_error),
                HAKO_PDU_ACTION_SERVER_EVENT_NONE);
            if (poll_error == HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL) {
                break;
            }
            ASSERT_EQ(poll_error, HAKO_PDU_ACTION_OK);
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(poll_error, HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL);
        EXPECT_GT(event_size, 0U);
    }
    OwnedBuffer server_packet;
    ASSERT_EQ(
        runtime.wait_server(server_info, server_packet),
        HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST);
    EXPECT_STREQ(server_info.action_name, kActionName);
    EXPECT_TRUE(same_goal(server_info.goal.goal_id, id));
    HakoCpp_FibonacciActionRequest request{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest request_convertor;
    ASSERT_TRUE(request_convertor.pdu2cpp(
        reinterpret_cast<char*>(server_packet.data), request));
    EXPECT_EQ(request.body.order, 8);

    ASSERT_EQ(
        hako_pdu_action_server_accept_goal(
            runtime.server(), kActionName, &server_info.goal),
        HAKO_PDU_ACTION_OK);

    hako_pdu_action_client_event_info_t client_info{};
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        hako_pdu_action_error_t poll_error = HAKO_PDU_ACTION_OK;
        std::size_t event_size = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            EXPECT_EQ(
                hako_pdu_action_client_poll(
                    runtime.client(),
                    &client_info,
                    nullptr,
                    0,
                    &event_size,
                    &poll_error),
                HAKO_PDU_ACTION_CLIENT_EVENT_NONE);
            if (poll_error == HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL) {
                break;
            }
            ASSERT_EQ(poll_error, HAKO_PDU_ACTION_OK);
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(poll_error, HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL);
        EXPECT_GT(event_size, 0U);
    }
    OwnedBuffer client_packet;
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE);
    EXPECT_EQ(client_info.decision, HAKO_PDU_ACTION_DECISION_ACCEPTED);
    EXPECT_TRUE(same_goal(client_info.goal.goal_id, id));

    auto feedback = runtime.feedback({0, 1, 1, 2, 3});
    ASSERT_NE(feedback.data, nullptr);
    ASSERT_EQ(
        hako_pdu_action_server_send_feedback(
            runtime.server(),
            kActionName,
            &server_info.goal,
            feedback.data,
            feedback.size),
        HAKO_PDU_ACTION_OK);
    client_packet = {};
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_FEEDBACK);
    HakoCpp_FibonacciActionFeedback decoded_feedback{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback feedback_convertor;
    ASSERT_TRUE(feedback_convertor.pdu2cpp(
        reinterpret_cast<char*>(client_packet.data), decoded_feedback));
    EXPECT_EQ(
        decoded_feedback.body.partial_sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2, 3}));

    auto result = runtime.result({0, 1, 1, 2, 3, 5, 8, 13});
    ASSERT_NE(result.data, nullptr);
    ASSERT_EQ(
        hako_pdu_action_server_complete(
            runtime.server(),
            kActionName,
            &server_info.goal,
            HAKO_PDU_ACTION_TERMINAL_SUCCEEDED,
            result.data,
            result.size),
        HAKO_PDU_ACTION_OK);
    client_packet = {};
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_RESULT);
    EXPECT_EQ(
        client_info.terminal_status,
        HAKO_PDU_ACTION_TERMINAL_SUCCEEDED);
    HakoCpp_FibonacciActionResponse decoded_result{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse result_convertor;
    ASSERT_TRUE(result_convertor.pdu2cpp(
        reinterpret_cast<char*>(client_packet.data), decoded_result));
    EXPECT_EQ(
        decoded_result.body.sequence,
        (std::vector<std::int32_t>{0, 1, 1, 2, 3, 5, 8, 13}));
}

TEST(CActionTcpE2EContract, GoalSendErrorsPreserveNativeReasons)
{
    CActionRuntime runtime;
    ASSERT_TRUE(runtime.start());

    hako_pdu_action_client_goal_handle_t goal{};
    const auto first_id = goal_id(0x11);
    ASSERT_EQ(
        runtime.send_goal_result(8, first_id, goal),
        HAKO_PDU_ACTION_OK);
    EXPECT_EQ(
        runtime.send_goal_result(8, first_id, goal),
        HAKO_PDU_ACTION_ERROR_DUPLICATE_GOAL);

    // action_resolved.json defines four communication slots. Fill the other
    // three without polling a response, then verify the fifth Goal is rejected
    // synchronously with the precise capacity error.
    ASSERT_EQ(
        runtime.send_goal_result(8, goal_id(0x21), goal),
        HAKO_PDU_ACTION_OK);
    ASSERT_EQ(
        runtime.send_goal_result(8, goal_id(0x31), goal),
        HAKO_PDU_ACTION_OK);
    ASSERT_EQ(
        runtime.send_goal_result(8, goal_id(0x41), goal),
        HAKO_PDU_ACTION_OK);
    EXPECT_EQ(
        runtime.send_goal_result(8, goal_id(0x51), goal),
        HAKO_PDU_ACTION_ERROR_NO_FREE_SLOT);
}

TEST(CActionTcpE2EContract, RejectedGoalDoesNotCreateServerGoal)
{
    CActionRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0x40);
    hako_pdu_action_client_goal_handle_t client_goal{};
    ASSERT_TRUE(runtime.send_goal(5, id, client_goal));

    hako_pdu_action_server_event_info_t server_info{};
    OwnedBuffer server_packet;
    ASSERT_EQ(
        runtime.wait_server(server_info, server_packet),
        HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST);
    ASSERT_EQ(
        hako_pdu_action_server_reject_goal(
            runtime.server(), kActionName, &server_info.goal),
        HAKO_PDU_ACTION_OK);

    hako_pdu_action_client_event_info_t client_info{};
    OwnedBuffer client_packet;
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE);
    EXPECT_EQ(client_info.decision, HAKO_PDU_ACTION_DECISION_REJECTED);
    EXPECT_TRUE(same_goal(client_info.goal.goal_id, id));

    auto feedback = runtime.feedback({0, 1});
    ASSERT_NE(feedback.data, nullptr);
    EXPECT_EQ(
        hako_pdu_action_server_send_feedback(
            runtime.server(),
            kActionName,
            &server_info.goal,
            feedback.data,
            feedback.size),
        HAKO_PDU_ACTION_ERROR_INVALID_STATE);
}

TEST(CActionTcpE2EContract, AcceptedCancelEndsWithCanceledResult)
{
    CActionRuntime runtime;
    ASSERT_TRUE(runtime.start());

    const auto id = goal_id(0x70);
    hako_pdu_action_client_goal_handle_t client_goal{};
    ASSERT_TRUE(runtime.send_goal(20, id, client_goal));

    hako_pdu_action_server_event_info_t server_info{};
    OwnedBuffer server_packet;
    ASSERT_EQ(
        runtime.wait_server(server_info, server_packet),
        HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST);
    ASSERT_EQ(
        hako_pdu_action_server_accept_goal(
            runtime.server(), kActionName, &server_info.goal),
        HAKO_PDU_ACTION_OK);

    hako_pdu_action_client_event_info_t client_info{};
    OwnedBuffer client_packet;
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE);
    ASSERT_EQ(client_info.decision, HAKO_PDU_ACTION_DECISION_ACCEPTED);

    ASSERT_EQ(
        hako_pdu_action_client_cancel_goal(
            runtime.client(), kActionName, &client_goal),
        HAKO_PDU_ACTION_OK);
    server_packet = {};
    ASSERT_EQ(
        runtime.wait_server(server_info, server_packet),
        HAKO_PDU_ACTION_SERVER_EVENT_CANCEL_REQUEST);
    ASSERT_EQ(
        hako_pdu_action_server_accept_cancel(
            runtime.server(), kActionName, &server_info.goal),
        HAKO_PDU_ACTION_OK);

    client_packet = {};
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_CANCEL_RESPONSE);
    EXPECT_EQ(client_info.decision, HAKO_PDU_ACTION_DECISION_ACCEPTED);

    auto result = runtime.result({0, 1, 1, 2, 3});
    ASSERT_NE(result.data, nullptr);
    ASSERT_EQ(
        hako_pdu_action_server_complete(
            runtime.server(),
            kActionName,
            &server_info.goal,
            HAKO_PDU_ACTION_TERMINAL_CANCELED,
            result.data,
            result.size),
        HAKO_PDU_ACTION_OK);
    client_packet = {};
    ASSERT_EQ(
        runtime.wait_client(client_info, client_packet),
        HAKO_PDU_ACTION_CLIENT_EVENT_RESULT);
    EXPECT_EQ(
        client_info.terminal_status,
        HAKO_PDU_ACTION_TERMINAL_CANCELED);
}

} // namespace
