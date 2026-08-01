#include <gtest/gtest.h>

#include "hakoniwa/pdu/rpc/c_rpc.h"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
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

    ~RuntimeGuard() {
        // The client side must be released before the server side so an
        // in-flight receive loop is not left waiting on a vanished peer.
        if (client != nullptr) {
            (void)hako_pdu_rpc_client_stop(client);
            hako_pdu_rpc_client_destroy(client);
        }
        if (server != nullptr) {
            (void)hako_pdu_rpc_server_stop(server);
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
            client,
            kServiceName,
            request.data(),
            request.size(),
            1'000'000);
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

hako_pdu_rpc_server_event_t wait_server_request(
    hako_pdu_rpc_server_handle_t* server,
    hako_pdu_rpc_request_info_t& info,
    std::vector<uint8_t>& pdu)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        size_t size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_server_poll(
            server,
            &info,
            pdu.data(),
            pdu.size(),
            &size,
            &error);
        if (error != HAKO_PDU_RPC_OK) {
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }
        if (event != HAKO_PDU_RPC_SERVER_EVENT_NONE) {
            pdu.resize(size);
            return event;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return HAKO_PDU_RPC_SERVER_EVENT_NONE;
}

hako_pdu_rpc_client_event_t wait_client_response(
    hako_pdu_rpc_client_handle_t* client,
    hako_pdu_rpc_response_info_t& info,
    std::vector<uint8_t>& pdu)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        size_t size = 0;
        hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
        const auto event = hako_pdu_rpc_client_poll(
            client,
            &info,
            pdu.data(),
            pdu.size(),
            &size,
            &error);
        if (error != HAKO_PDU_RPC_OK) {
            return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
        }
        if (event != HAKO_PDU_RPC_CLIENT_EVENT_NONE) {
            pdu.resize(size);
            return event;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
}

TEST(CRpcRoundTripTest, AddTwoIntsThroughCFacade) {
    RuntimeGuard runtime;
    runtime.server = hako_pdu_rpc_server_create(
        kServerNodeId,
        kServiceConfigPath,
        kEndpointConfigPath,
        1000,
        "real");
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
    request_cpp.body.a = 5;
    request_cpp.body.b = 7;
    const int encoded_request_size = request_converter.cpp2pdu(
        request_cpp,
        reinterpret_cast<char*>(request.data()),
        static_cast<int>(request.size()));
    ASSERT_GT(encoded_request_size, 0);
    request.resize(static_cast<size_t>(encoded_request_size));

    ASSERT_TRUE(call_when_connected(runtime.client, request));

    hako_pdu_rpc_request_info_t request_info{};
    std::vector<uint8_t> received_request(kMaxPduSize);
    ASSERT_EQ(
        wait_server_request(runtime.server, request_info, received_request),
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN);
    EXPECT_STREQ(request_info.service_name, kServiceName);
    EXPECT_STREQ(request_info.client_name, kClientName);

    HakoCpp_AddTwoIntsRequestPacket received_request_cpp{};
    ASSERT_TRUE(request_converter.pdu2cpp(
        reinterpret_cast<char*>(received_request.data()), received_request_cpp));
    EXPECT_EQ(received_request_cpp.body.a, 5);
    EXPECT_EQ(received_request_cpp.body.b, 7);

    size_t reply_size = 0;
    ASSERT_EQ(
        hako_pdu_rpc_server_create_reply_buffer(
            runtime.server,
            request_info.request_token,
            static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE),
            static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK),
            nullptr,
            0,
            &reply_size),
        HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> reply(reply_size);
    ASSERT_EQ(
        hako_pdu_rpc_server_create_reply_buffer(
            runtime.server,
            request_info.request_token,
            static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE),
            static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK),
            reply.data(),
            reply.size(),
            &reply_size),
        HAKO_PDU_RPC_OK);
    reply.resize(reply_size);

    AddTwoIntsResponsePacket response_converter;
    HakoCpp_AddTwoIntsResponsePacket response_cpp{};
    ASSERT_TRUE(response_converter.pdu2cpp(
        reinterpret_cast<char*>(reply.data()), response_cpp));
    response_cpp.body.sum = received_request_cpp.body.a + received_request_cpp.body.b;
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

    hako_pdu_rpc_response_info_t response_info{};
    std::vector<uint8_t> received_response(kMaxPduSize);
    ASSERT_EQ(
        wait_client_response(runtime.client, response_info, received_response),
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN);
    EXPECT_STREQ(response_info.service_name, kServiceName);

    HakoCpp_AddTwoIntsResponsePacket received_response_cpp{};
    ASSERT_TRUE(response_converter.pdu2cpp(
        reinterpret_cast<char*>(received_response.data()), received_response_cpp));
    EXPECT_EQ(received_response_cpp.body.sum, 12);
}

} // namespace
