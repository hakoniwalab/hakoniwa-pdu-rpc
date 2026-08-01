#include <gtest/gtest.h>

#include "hakoniwa/pdu/rpc/c_rpc.h"
#include "hakoniwa/pdu/rpc/rpc_types.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsResponsePacket;

constexpr const char* kServiceConfig = "configs/service_config.json";
constexpr const char* kEndpointConfig = "configs/endpoints.json";
constexpr const char* kService = "Service/Add";
constexpr size_t kMaxPdu = 4 * 1024 * 1024;

struct Runtime {
    hako_pdu_rpc_server_handle_t* server{nullptr};
    hako_pdu_rpc_client_handle_t* client{nullptr};

    ~Runtime()
    {
        if (server && client) (void)hako_pdu_rpc_stop_pair(server, client);
        if (client) hako_pdu_rpc_client_destroy(client);
        if (server) hako_pdu_rpc_server_destroy(server);
    }

    bool start()
    {
        server = hako_pdu_rpc_server_create(
            "server_node", kServiceConfig, kEndpointConfig, 1000, "real");
        client = hako_pdu_rpc_client_create(
            "client_node", "TestClient", kServiceConfig, kEndpointConfig, 1000, "real");
        return server && client
            && hako_pdu_rpc_server_start(server) == HAKO_PDU_RPC_OK
            && hako_pdu_rpc_client_start(client) == HAKO_PDU_RPC_OK;
    }
};

std::vector<uint8_t> make_request(Runtime& runtime, int64_t a, int64_t b)
{
    size_t size = 0;
    EXPECT_EQ(hako_pdu_rpc_client_create_request_buffer(
        runtime.client, kService, nullptr, 0, &size), HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pdu(size);
    EXPECT_EQ(hako_pdu_rpc_client_create_request_buffer(
        runtime.client, kService, pdu.data(), pdu.size(), &size), HAKO_PDU_RPC_OK);
    pdu.resize(size);

    AddTwoIntsRequestPacket converter;
    HakoCpp_AddTwoIntsRequestPacket cpp{};
    EXPECT_TRUE(converter.pdu2cpp(reinterpret_cast<char*>(pdu.data()), cpp));
    cpp.body.a = a;
    cpp.body.b = b;
    const int encoded = converter.cpp2pdu(
        cpp, reinterpret_cast<char*>(pdu.data()), static_cast<int>(pdu.size()));
    EXPECT_GT(encoded, 0);
    pdu.resize(static_cast<size_t>(encoded));
    return pdu;
}

bool call_when_connected(Runtime& runtime, const std::vector<uint8_t>& pdu, uint64_t timeout_usec)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        const auto result = hako_pdu_rpc_client_call(
            runtime.client, kService, pdu.data(), pdu.size(), timeout_usec);
        if (result == HAKO_PDU_RPC_OK) return true;
        if (result != HAKO_PDU_RPC_ERROR_CALL) return false;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

hako_pdu_rpc_server_event_t wait_server(
    Runtime& runtime,
    hako_pdu_rpc_request_info_t& info,
    std::vector<uint8_t>& pdu)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        size_t size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_server_poll(
            runtime.server, &info, pdu.data(), pdu.size(), &size, &error);
        EXPECT_EQ(error, HAKO_PDU_RPC_OK);
        if (event != HAKO_PDU_RPC_SERVER_EVENT_NONE) {
            pdu.resize(size);
            return event;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return HAKO_PDU_RPC_SERVER_EVENT_NONE;
}

hako_pdu_rpc_client_event_t wait_client(
    Runtime& runtime,
    hako_pdu_rpc_response_info_t& info,
    std::vector<uint8_t>& pdu)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        size_t size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_client_poll(
            runtime.client, &info, pdu.data(), pdu.size(), &size, &error);
        EXPECT_EQ(error, HAKO_PDU_RPC_OK);
        if (event != HAKO_PDU_RPC_CLIENT_EVENT_NONE) {
            pdu.resize(size);
            return event;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
}

void send_normal_reply(Runtime& runtime, uint64_t token, int64_t sum)
{
    const auto done = static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE);
    const auto ok = static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK);
    size_t size = 0;
    ASSERT_EQ(hako_pdu_rpc_server_create_reply_buffer(
        runtime.server, token, done, ok, nullptr, 0, &size), HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> reply(size);
    ASSERT_EQ(hako_pdu_rpc_server_create_reply_buffer(
        runtime.server, token, done, ok, reply.data(), reply.size(), &size), HAKO_PDU_RPC_OK);
    reply.resize(size);

    AddTwoIntsResponsePacket converter;
    HakoCpp_AddTwoIntsResponsePacket cpp{};
    ASSERT_TRUE(converter.pdu2cpp(reinterpret_cast<char*>(reply.data()), cpp));
    cpp.body.sum = sum;
    const int encoded = converter.cpp2pdu(
        cpp, reinterpret_cast<char*>(reply.data()), static_cast<int>(reply.size()));
    ASSERT_GT(encoded, 0);
    reply.resize(static_cast<size_t>(encoded));
    ASSERT_EQ(hako_pdu_rpc_server_send_reply(
        runtime.server, token, reply.data(), reply.size()), HAKO_PDU_RPC_OK);
}

int64_t parse_sum(const std::vector<uint8_t>& pdu)
{
    AddTwoIntsResponsePacket converter;
    HakoCpp_AddTwoIntsResponsePacket cpp{};
    EXPECT_TRUE(converter.pdu2cpp(
        reinterpret_cast<char*>(const_cast<uint8_t*>(pdu.data())), cpp));
    return cpp.body.sum;
}

TEST(CRpcCancelRaceContractTest, NormalResponseMayWinAfterExplicitCancel)
{
    Runtime runtime;
    ASSERT_TRUE(runtime.start());

    auto request = make_request(runtime, 3, 4);
    ASSERT_TRUE(call_when_connected(runtime, request, 50'000));

    hako_pdu_rpc_request_info_t original_info{};
    std::vector<uint8_t> original_pdu(kMaxPdu);
    ASSERT_EQ(wait_server(runtime, original_info, original_pdu),
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN);

    hako_pdu_rpc_response_info_t response_info{};
    std::vector<uint8_t> response(kMaxPdu);
    ASSERT_EQ(wait_client(runtime, response_info, response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT);
    ASSERT_STREQ(response_info.service_name, kService);

    ASSERT_EQ(hako_pdu_rpc_client_cancel(runtime.client, kService), HAKO_PDU_RPC_OK);

    // Complete the original work before the server polls the queued cancel.
    send_normal_reply(runtime, original_info.request_token, 7);

    response_info = {};
    response.assign(kMaxPdu, 0);
    ASSERT_EQ(wait_client(runtime, response_info, response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN);
    ASSERT_STREQ(response_info.service_name, kService);
    ASSERT_EQ(parse_sum(response), 7);

    // The late cancel must be harmless and must not poison the next request.
    auto next_request = make_request(runtime, 8, 9);
    ASSERT_TRUE(call_when_connected(runtime, next_request, 1'000'000));

    hako_pdu_rpc_request_info_t next_info{};
    std::vector<uint8_t> next_pdu(kMaxPdu);
    ASSERT_EQ(wait_server(runtime, next_info, next_pdu),
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN);
    ASSERT_NE(next_info.request_token, original_info.request_token);

    send_normal_reply(runtime, next_info.request_token, 17);

    response_info = {};
    response.assign(kMaxPdu, 0);
    ASSERT_EQ(wait_client(runtime, response_info, response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN);
    ASSERT_STREQ(response_info.service_name, kService);
    EXPECT_EQ(parse_sum(response), 17);
}

} // namespace
