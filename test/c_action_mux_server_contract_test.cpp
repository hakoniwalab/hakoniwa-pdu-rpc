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
    ~OwnedBuffer() { hako_pdu_action_buffer_free(data); }
};

hako_pdu_action_goal_id_t make_goal_id(std::uint8_t seed)
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

class CActionMuxRuntime {
public:
    ~CActionMuxRuntime() { stop(); }

    bool start()
    {
        server_ = hako_pdu_action_mux_server_create(
            "fibonacci-server",
            ACTION_C_MUX_CONFIG_FIXTURE_PATH,
            ACTION_C_MUX_SERVER_ENDPOINT_FIXTURE_PATH,
            1000,
            "real");
        client0_ = hako_pdu_action_client_create(
            "fibonacci-client",
            "c-action-mux-client-0",
            ACTION_C_MUX_CONFIG_FIXTURE_PATH,
            ACTION_C_MUX_CLIENT_ENDPOINTS_FIXTURE_PATH,
            1000,
            "real");
        client1_ = hako_pdu_action_client_create(
            "fibonacci-client",
            "c-action-mux-client-1",
            ACTION_C_MUX_CONFIG_FIXTURE_PATH,
            ACTION_C_MUX_CLIENT_ENDPOINTS_FIXTURE_PATH,
            1000,
            "real");
        if (server_ == nullptr || client0_ == nullptr || client1_ == nullptr) {
            return false;
        }
        if (hako_pdu_action_mux_server_start(server_) != HAKO_PDU_ACTION_OK
            || hako_pdu_action_client_start(client0_) != HAKO_PDU_ACTION_OK
            || hako_pdu_action_client_start(client1_) != HAKO_PDU_ACTION_OK) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            hako_pdu_action_server_event_info_t info{};
            OwnedBuffer packet;
            hako_pdu_action_error_t error = HAKO_PDU_ACTION_OK;
            (void)hako_pdu_action_mux_server_poll_alloc(
                server_, &info, &packet.data, &packet.size, &error);
            if (error != HAKO_PDU_ACTION_OK) {
                return false;
            }
            if (hako_pdu_action_mux_server_is_ready(server_) != 0) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void stop()
    {
        if (client0_ != nullptr) {
            (void)hako_pdu_action_client_stop(client0_);
            hako_pdu_action_client_destroy(client0_);
            client0_ = nullptr;
        }
        if (client1_ != nullptr) {
            (void)hako_pdu_action_client_stop(client1_);
            hako_pdu_action_client_destroy(client1_);
            client1_ = nullptr;
        }
        if (server_ != nullptr) {
            (void)hako_pdu_action_mux_server_stop(server_);
            hako_pdu_action_mux_server_destroy(server_);
            server_ = nullptr;
        }
    }

    hako_pdu_action_client_handle_t* client(std::size_t index)
    {
        return index == 0 ? client0_ : client1_;
    }

    hako_pdu_action_mux_server_handle_t* server() { return server_; }

private:
    hako_pdu_action_mux_server_handle_t* server_{nullptr};
    hako_pdu_action_client_handle_t* client0_{nullptr};
    hako_pdu_action_client_handle_t* client1_{nullptr};
};

OwnedBuffer make_goal_packet(
    hako_pdu_action_client_handle_t* client,
    std::int32_t order)
{
    OwnedBuffer packet;
    if (hako_pdu_action_client_create_goal_buffer_alloc(
            client, kActionName, &packet.data, &packet.size)
        != HAKO_PDU_ACTION_OK) {
        return {};
    }
    HakoCpp_FibonacciActionRequest request{};
    request.body.order = order;
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest convertor;
    if (convertor.cpp2pdu(
            request,
            reinterpret_cast<char*>(packet.data),
            static_cast<int>(packet.size)) <= 0) {
        return {};
    }
    return packet;
}

OwnedBuffer make_result_packet(
    hako_pdu_action_mux_server_handle_t* server,
    std::vector<std::int32_t> sequence)
{
    OwnedBuffer packet;
    if (hako_pdu_action_mux_server_create_result_buffer_alloc(
            server, kActionName, &packet.data, &packet.size)
        != HAKO_PDU_ACTION_OK) {
        return {};
    }
    HakoCpp_FibonacciActionResponse response{};
    response.body.sequence = std::move(sequence);
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    if (convertor.cpp2pdu(
            response,
            reinterpret_cast<char*>(packet.data),
            static_cast<int>(packet.size)) <= 0) {
        return {};
    }
    return packet;
}

OwnedBuffer make_feedback_packet(
    hako_pdu_action_mux_server_handle_t* server,
    std::vector<std::int32_t> sequence)
{
    OwnedBuffer packet;
    if (hako_pdu_action_mux_server_create_feedback_buffer_alloc(
            server, kActionName, &packet.data, &packet.size)
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

hako_pdu_action_server_event_t wait_server(
    hako_pdu_action_mux_server_handle_t* server,
    hako_pdu_action_server_event_info_t& info,
    OwnedBuffer& packet)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        hako_pdu_action_error_t error = HAKO_PDU_ACTION_OK;
        const auto event = hako_pdu_action_mux_server_poll_alloc(
            server, &info, &packet.data, &packet.size, &error);
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
    hako_pdu_action_client_handle_t* client,
    hako_pdu_action_client_event_info_t& info,
    OwnedBuffer& packet)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        hako_pdu_action_error_t error = HAKO_PDU_ACTION_OK;
        const auto event = hako_pdu_action_client_poll_alloc(
            client, &info, &packet.data, &packet.size, &error);
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

TEST(CActionMuxServerContract, RoutesTwoClientsWithoutConnectionIdentity)
{
    CActionMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());
    EXPECT_EQ(hako_pdu_action_mux_server_expected_count(runtime.server()), 2U);
    EXPECT_EQ(hako_pdu_action_mux_server_connected_count(runtime.server()), 2U);

    hako_pdu_action_server_event_info_t server_events[2]{};
    hako_pdu_action_client_goal_handle_t client_goals[2]{};
    const auto ids = std::vector{
        make_goal_id(0x20),
        make_goal_id(0x50),
    };

    for (std::size_t index = 0; index < ids.size(); ++index) {
        auto packet = make_goal_packet(
            runtime.client(index), static_cast<std::int32_t>(8 + index));
        ASSERT_NE(packet.data, nullptr);
        ASSERT_EQ(
            hako_pdu_action_client_send_goal(
                runtime.client(index),
                kActionName,
                packet.data,
                packet.size,
                &ids[index],
                &client_goals[index],
                1'000'000),
            HAKO_PDU_ACTION_OK);

        OwnedBuffer request;
        ASSERT_EQ(
            wait_server(runtime.server(), server_events[index], request),
            HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST);
        EXPECT_STREQ(server_events[index].action_name, kActionName);
        EXPECT_TRUE(same_goal(server_events[index].goal.goal_id, ids[index]));
        ASSERT_EQ(
            hako_pdu_action_mux_server_accept_goal(
                runtime.server(),
                server_events[index].action_name,
                &server_events[index].goal),
            HAKO_PDU_ACTION_OK);

        hako_pdu_action_client_event_info_t response_info{};
        OwnedBuffer response;
        ASSERT_EQ(
            wait_client(runtime.client(index), response_info, response),
            HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE);
        EXPECT_EQ(response_info.decision, HAKO_PDU_ACTION_DECISION_ACCEPTED);
    }

    auto feedback = make_feedback_packet(runtime.server(), {0, 1, 1, 2, 3});
    ASSERT_NE(feedback.data, nullptr);
    ASSERT_EQ(
        hako_pdu_action_mux_server_send_feedback(
            runtime.server(),
            server_events[0].action_name,
            &server_events[0].goal,
            feedback.data,
            feedback.size),
        HAKO_PDU_ACTION_OK);
    hako_pdu_action_client_event_info_t feedback_info{};
    OwnedBuffer feedback_event;
    ASSERT_EQ(
        wait_client(runtime.client(0), feedback_info, feedback_event),
        HAKO_PDU_ACTION_CLIENT_EVENT_FEEDBACK);
    EXPECT_TRUE(same_goal(feedback_info.goal.goal_id, ids[0]));

    ASSERT_EQ(
        hako_pdu_action_client_cancel_goal(
            runtime.client(1), kActionName, &client_goals[1]),
        HAKO_PDU_ACTION_OK);
    hako_pdu_action_server_event_info_t cancel_info{};
    OwnedBuffer cancel_packet;
    ASSERT_EQ(
        wait_server(runtime.server(), cancel_info, cancel_packet),
        HAKO_PDU_ACTION_SERVER_EVENT_CANCEL_REQUEST);
    EXPECT_TRUE(same_goal(cancel_info.goal.goal_id, ids[1]));
    ASSERT_EQ(
        hako_pdu_action_mux_server_accept_cancel(
            runtime.server(), cancel_info.action_name, &cancel_info.goal),
        HAKO_PDU_ACTION_OK);
    hako_pdu_action_client_event_info_t cancel_response_info{};
    OwnedBuffer cancel_response;
    ASSERT_EQ(
        wait_client(runtime.client(1), cancel_response_info, cancel_response),
        HAKO_PDU_ACTION_CLIENT_EVENT_CANCEL_RESPONSE);
    EXPECT_EQ(
        cancel_response_info.decision,
        HAKO_PDU_ACTION_DECISION_ACCEPTED);

    for (std::size_t index = 0; index < ids.size(); ++index) {
        auto result = make_result_packet(
            runtime.server(), {0, 1, static_cast<std::int32_t>(index + 1)});
        ASSERT_NE(result.data, nullptr);
        ASSERT_EQ(
            hako_pdu_action_mux_server_complete(
                runtime.server(),
                server_events[index].action_name,
                &server_events[index].goal,
                index == 0
                    ? HAKO_PDU_ACTION_TERMINAL_SUCCEEDED
                    : HAKO_PDU_ACTION_TERMINAL_CANCELED,
                result.data,
                result.size),
            HAKO_PDU_ACTION_OK);

        hako_pdu_action_client_event_info_t result_info{};
        OwnedBuffer result_event;
        ASSERT_EQ(
            wait_client(runtime.client(index), result_info, result_event),
            HAKO_PDU_ACTION_CLIENT_EVENT_RESULT);
        EXPECT_TRUE(same_goal(result_info.goal.goal_id, ids[index]));
        EXPECT_EQ(
            result_info.terminal_status,
            index == 0
                ? HAKO_PDU_ACTION_TERMINAL_SUCCEEDED
                : HAKO_PDU_ACTION_TERMINAL_CANCELED);
    }
}

} // namespace
