#include <gtest/gtest.h>

#include "hakoniwa/pdu/rpc/c_rpc.h"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsResponsePacket;

constexpr const char* kServiceConfigPath = "configs/service_config.json";
constexpr const char* kEndpointConfigPath = "configs/endpoints.json";
constexpr const char* kServerNodeId = "server_node";
constexpr const char* kClientNodeId = "client_node";
constexpr const char* kClientName = "TestClient";
constexpr const char* kServiceName = "Service/Add";
constexpr size_t kMaxPduSize = 4 * 1024 * 1024;

struct RuntimeGuard {
    hako_pdu_rpc_server_handle_t* server{nullptr};
    hako_pdu_rpc_client_handle_t* client{nullptr};

    ~RuntimeGuard()
    {
        if (server != nullptr && client != nullptr) {
            (void)hako_pdu_rpc_stop_pair(server, client);
        }
        if (client != nullptr) {
            hako_pdu_rpc_client_destroy(client);
        }
        if (server != nullptr) {
            hako_pdu_rpc_server_destroy(server);
        }
    }
};

bool call_when_connected(
    hako_pdu_rpc_client_handle_t* client,
    const std::vector<uint8_t>& request)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        const auto result = hako_pdu_rpc_client_call(
            client, kServiceName, request.data(), request.size(), 0);
        if (result == HAKO_PDU_RPC_OK) {
            return true;
        }
        if (result != HAKO_PDU_RPC_ERROR_CALL) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST(CRpcInfiniteWaitContractTest, TimeoutZeroDoesNotEmitTimeoutBeforeReply)
{
    RuntimeGuard runtime;
    runtime.server = hako_pdu_rpc_server_create(
        kServerNodeId, kServiceConfigPath, kEndpointConfigPath, 1000, "real");
    ASSERT_NE(runtime.server, nullptr);

    runtime.client = hako_pdu_rpc_client_create(
        kClientNodeId,
        kClientName,
        kServiceConfigPath,
        kEndpointConfigPath,
        1000,
        "real");
    ASSERT_NE(runtime.client, nullptr);

    ASSERT_EQ(hako_pdu_rpc_server_start(runtime.server), HAKO_PDU_RPC_OK);
    ASSERT_EQ(hako_pdu_rpc_client_start(runtime.client), HAKO_PDU_RPC_OK);

    size_t request_size = 0;
    ASSERT_EQ(
        hako_pdu_rpc_client_create_request_buffer(
            runtime.client, kServiceName, nullptr, 0, &request_size),
        HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> request(request_size);
    ASSERT_EQ(
        hako_pdu_rpc_client_create_request_buffer(
            runtime.client, kServiceName, request.data(), request.size(), &request_size),
        HAKO_PDU_RPC_OK);
    request.resize(request_size);

    AddTwoIntsRequestPacket request_converter;
    HakoCpp_AddTwoIntsRequestPacket request_cpp{};
    ASSERT_TRUE(request_converter.pdu2cpp(
        reinterpret_cast<char*>(request.data()), request_cpp));
    request_cpp.body.a = 99;
    request_cpp.body.b = 1;
    const int encoded_request_size = request_converter.cpp2pdu(
        request_cpp,
        reinterpret_cast<char*>(request.data()),
        static_cast<int>(request.size()));
    ASSERT_GT(encoded_request_size, 0);
    request.resize(static_cast<size_t>(encoded_request_size));

    ASSERT_TRUE(call_when_connected(runtime.client, request));

    hako_pdu_rpc_request_info_t request_info{};
    std::vector<uint8_t> received_request(kMaxPduSize);
    const auto server_deadline = std::chrono::steady_clock::now() + 3s;
    for (;;) {
        size_t received_size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_server_poll(
            runtime.server,
            &request_info,
            received_request.data(),
            received_request.size(),
            &received_size,
            &error);
        ASSERT_EQ(error, HAKO_PDU_RPC_OK);
        if (event == HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN) {
            received_request.resize(received_size);
            break;
        }
        ASSERT_EQ(event, HAKO_PDU_RPC_SERVER_EVENT_NONE);
        ASSERT_LT(std::chrono::steady_clock::now(), server_deadline);
        std::this_thread::sleep_for(1ms);
    }

    for (int i = 0; i < 100; ++i) {
        hako_pdu_rpc_response_info_t response_info{};
        std::vector<uint8_t> response(kMaxPduSize);
        size_t response_size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_client_poll(
            runtime.client,
            &response_info,
            response.data(),
            response.size(),
            &response_size,
            &error);
        EXPECT_EQ(error, HAKO_PDU_RPC_OK);
        EXPECT_EQ(event, HAKO_PDU_RPC_CLIENT_EVENT_NONE);
        std::this_thread::sleep_for(1ms);
    }

    HakoCpp_AddTwoIntsRequestPacket received_request_cpp{};
    ASSERT_TRUE(request_converter.pdu2cpp(
        reinterpret_cast<char*>(received_request.data()), received_request_cpp));
    ASSERT_EQ(received_request_cpp.body.a, 99);
    ASSERT_EQ(received_request_cpp.body.b, 1);

    size_t reply_size = 0;
    ASSERT_EQ(
        hako_pdu_rpc_server_create_reply_buffer(
            runtime.server,
            request_info.request_token,
            HAKO_PDU_RPC_SERVICE_STATUS_DONE,
            HAKO_PDU_RPC_SERVICE_RESULT_OK,
            nullptr,
            0,
            &reply_size),
        HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> reply(reply_size);
    ASSERT_EQ(
        hako_pdu_rpc_server_create_reply_buffer(
            runtime.server,
            request_info.request_token,
            HAKO_PDU_RPC_SERVICE_STATUS_DONE,
            HAKO_PDU_RPC_SERVICE_RESULT_OK,
            reply.data(),
            reply.size(),
            &reply_size),
        HAKO_PDU_RPC_OK);
    reply.resize(reply_size);

    AddTwoIntsResponsePacket response_converter;
    HakoCpp_AddTwoIntsResponsePacket response_cpp{};
    ASSERT_TRUE(response_converter.pdu2cpp(
        reinterpret_cast<char*>(reply.data()), response_cpp));
    response_cpp.body.sum = 100;
    const int encoded_reply_size = response_converter.cpp2pdu(
        response_cpp,
        reinterpret_cast<char*>(reply.data()),
        static_cast<int>(reply.size()));
    ASSERT_GT(encoded_reply_size, 0);
    reply.resize(static_cast<size_t>(encoded_reply_size));

    ASSERT_EQ(
        hako_pdu_rpc_server_send_reply(
            runtime.server,
            request_info.request_token,
            reply.data(),
            reply.size()),
        HAKO_PDU_RPC_OK);

    const auto client_deadline = std::chrono::steady_clock::now() + 3s;
    for (;;) {
        hako_pdu_rpc_response_info_t response_info{};
        std::vector<uint8_t> response(kMaxPduSize);
        size_t response_size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_client_poll(
            runtime.client,
            &response_info,
            response.data(),
            response.size(),
            &response_size,
            &error);
        ASSERT_EQ(error, HAKO_PDU_RPC_OK);
        if (event == HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN) {
            response.resize(response_size);
            HakoCpp_AddTwoIntsResponsePacket parsed{};
            ASSERT_TRUE(response_converter.pdu2cpp(
                reinterpret_cast<char*>(response.data()), parsed));
            EXPECT_EQ(parsed.body.sum, 100);
            break;
        }
        ASSERT_EQ(event, HAKO_PDU_RPC_CLIENT_EVENT_NONE);
        ASSERT_LT(std::chrono::steady_clock::now(), client_deadline);
        std::this_thread::sleep_for(1ms);
    }
}

} // namespace
