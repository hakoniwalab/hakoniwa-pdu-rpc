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
        return server && client &&
            hako_pdu_rpc_server_start(server) == HAKO_PDU_RPC_OK &&
            hako_pdu_rpc_client_start(client) == HAKO_PDU_RPC_OK;
    }
};

std::vector<uint8_t> make_request(hako_pdu_rpc_client_handle_t* client, long long a, long long b)
{
    size_t size = 0;
    EXPECT_EQ(hako_pdu_rpc_client_create_request_buffer(client, kService, nullptr, 0, &size),
        HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pdu(size);
    EXPECT_EQ(hako_pdu_rpc_client_create_request_buffer(client, kService, pdu.data(), pdu.size(), &size),
        HAKO_PDU_RPC_OK);
    pdu.resize(size);

    AddTwoIntsRequestPacket converter;
    HakoCpp_AddTwoIntsRequestPacket value{};
    EXPECT_TRUE(converter.pdu2cpp(reinterpret_cast<char*>(pdu.data()), value));
    value.body.a = a;
    value.body.b = b;
    const int encoded = converter.cpp2pdu(
        value, reinterpret_cast<char*>(pdu.data()), static_cast<int>(pdu.size()));
    EXPECT_GT(encoded, 0);
    pdu.resize(static_cast<size_t>(encoded));
    return pdu;
}

bool call_when_connected(hako_pdu_rpc_client_handle_t* client, const std::vector<uint8_t>& pdu, uint64_t timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        const auto result = hako_pdu_rpc_client_call(client, kService, pdu.data(), pdu.size(), timeout);
        if (result == HAKO_PDU_RPC_OK) return true;
        if (result != HAKO_PDU_RPC_ERROR_CALL) return false;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

hako_pdu_rpc_server_event_t wait_server(
    hako_pdu_rpc_server_handle_t* server,
    hako_pdu_rpc_request_info_t& info,
    std::vector<uint8_t>& pdu)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        size_t size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_server_poll(
            server, &info, pdu.data(), pdu.size(), &size, &error);
        if (error != HAKO_PDU_RPC_OK) return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        if (event != HAKO_PDU_RPC_SERVER_EVENT_NONE) {
            pdu.resize(size);
            return event;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return HAKO_PDU_RPC_SERVER_EVENT_NONE;
}

hako_pdu_rpc_client_event_t wait_client(
    hako_pdu_rpc_client_handle_t* client,
    hako_pdu_rpc_response_info_t& info,
    std::vector<uint8_t>& pdu)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        size_t size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_client_poll(
            client, &info, pdu.data(), pdu.size(), &size, &error);
        if (error != HAKO_PDU_RPC_OK) return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
        if (event != HAKO_PDU_RPC_CLIENT_EVENT_NONE) {
            pdu.resize(size);
            return event;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
}

std::vector<uint8_t> make_reply(
    hako_pdu_rpc_server_handle_t* server,
    uint64_t token,
    int32_t result_code,
    long long sum)
{
    const auto done = static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE);
    size_t size = 0;
    EXPECT_EQ(hako_pdu_rpc_server_create_reply_buffer(
        server, token, done, result_code, nullptr, 0, &size),
        HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pdu(size);
    EXPECT_EQ(hako_pdu_rpc_server_create_reply_buffer(
        server, token, done, result_code, pdu.data(), pdu.size(), &size),
        HAKO_PDU_RPC_OK);
    pdu.resize(size);

    AddTwoIntsResponsePacket converter;
    HakoCpp_AddTwoIntsResponsePacket value{};
    EXPECT_TRUE(converter.pdu2cpp(reinterpret_cast<char*>(pdu.data()), value));
    value.body.sum = sum;
    const int encoded = converter.cpp2pdu(
        value, reinterpret_cast<char*>(pdu.data()), static_cast<int>(pdu.size()));
    EXPECT_GT(encoded, 0);
    pdu.resize(static_cast<size_t>(encoded));
    return pdu;
}

TEST(CRpcTimeoutCancelContractTest, TimeoutRequiresExplicitCancelAndTerminalCancelCompletion)
{
    Runtime runtime;
    ASSERT_TRUE(runtime.start());

    auto request = make_request(runtime.client, 10, 20);
    ASSERT_TRUE(call_when_connected(runtime.client, request, 50'000));

    hako_pdu_rpc_request_info_t original_info{};
    std::vector<uint8_t> original_pdu(kMaxPdu);
    ASSERT_EQ(wait_server(runtime.server, original_info, original_pdu),
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN);

    AddTwoIntsRequestPacket request_converter;
    HakoCpp_AddTwoIntsRequestPacket original_cpp{};
    ASSERT_TRUE(request_converter.pdu2cpp(
        reinterpret_cast<char*>(original_pdu.data()), original_cpp));

    hako_pdu_rpc_response_info_t response_info{};
    std::vector<uint8_t> response(kMaxPdu);
    ASSERT_EQ(wait_client(runtime.client, response_info, response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT);
    ASSERT_STREQ(response_info.service_name, kService);

    ASSERT_EQ(hako_pdu_rpc_client_cancel(runtime.client, kService), HAKO_PDU_RPC_OK);

    hako_pdu_rpc_request_info_t cancel_info{};
    std::vector<uint8_t> cancel_pdu(kMaxPdu);
    ASSERT_EQ(wait_server(runtime.server, cancel_info, cancel_pdu),
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL);

    HakoCpp_AddTwoIntsRequestPacket cancel_cpp{};
    ASSERT_TRUE(request_converter.pdu2cpp(
        reinterpret_cast<char*>(cancel_pdu.data()), cancel_cpp));
    ASSERT_EQ(cancel_cpp.header.request_id, original_cpp.header.request_id);

    const auto canceled = static_cast<int32_t>(
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_CANCELED);
    auto cancel_reply = make_reply(runtime.server, cancel_info.request_token, canceled, 0);
    ASSERT_EQ(hako_pdu_rpc_server_send_cancel_reply(
        runtime.server, cancel_info.request_token, cancel_reply.data(), cancel_reply.size()),
        HAKO_PDU_RPC_OK);

    response_info = {};
    response.assign(kMaxPdu, 0);
    ASSERT_EQ(wait_client(runtime.client, response_info, response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL);

    auto next_request = make_request(runtime.client, 7, 8);
    ASSERT_TRUE(call_when_connected(runtime.client, next_request, 1'000'000));

    hako_pdu_rpc_request_info_t next_info{};
    std::vector<uint8_t> next_pdu(kMaxPdu);
    ASSERT_EQ(wait_server(runtime.server, next_info, next_pdu),
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN);

    const auto ok = static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK);
    auto next_reply = make_reply(runtime.server, next_info.request_token, ok, 15);
    ASSERT_EQ(hako_pdu_rpc_server_send_reply(
        runtime.server, next_info.request_token, next_reply.data(), next_reply.size()),
        HAKO_PDU_RPC_OK);

    response_info = {};
    response.assign(kMaxPdu, 0);
    ASSERT_EQ(wait_client(runtime.client, response_info, response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN);

    AddTwoIntsResponsePacket response_converter;
    HakoCpp_AddTwoIntsResponsePacket parsed{};
    ASSERT_TRUE(response_converter.pdu2cpp(reinterpret_cast<char*>(response.data()), parsed));
    EXPECT_EQ(parsed.body.sum, 15);
}

} // namespace
