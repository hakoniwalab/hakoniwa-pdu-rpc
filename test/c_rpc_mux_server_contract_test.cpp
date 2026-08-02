#include <gtest/gtest.h>

#include "hakoniwa/pdu/rpc/c_rpc.h"
#include "hakoniwa/pdu/rpc/rpc_types.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsResponsePacket;

constexpr const char* kServiceConfig = "configs/service_config_mux.json";
constexpr const char* kClientEndpointConfig = "configs/endpoints_mux_clients.json";
constexpr const char* kMuxEndpointConfig = "configs/mux_server_endpoint.json";
constexpr const char* kService = "Service/Add";
constexpr size_t kMaxPdu = 4 * 1024 * 1024;

struct PendingCall {
    hako_pdu_rpc_request_info_t info{};
    std::vector<uint8_t> pdu;
};

class CMuxRuntime {
public:
    ~CMuxRuntime()
    {
        stop();
    }

    bool start()
    {
        server_ = hako_pdu_rpc_mux_server_create(
            "server_node", kServiceConfig, kMuxEndpointConfig, 1000, "real");
        client0_ = hako_pdu_rpc_client_create(
            "client_node", "TestClient0", kServiceConfig,
            kClientEndpointConfig, 1000, "real");
        client1_ = hako_pdu_rpc_client_create(
            "client_node", "TestClient1", kServiceConfig,
            kClientEndpointConfig, 1000, "real");
        if (!server_ || !client0_ || !client1_) {
            return false;
        }
        if (hako_pdu_rpc_mux_server_start(server_) != HAKO_PDU_RPC_OK
            || hako_pdu_rpc_client_start(client0_) != HAKO_PDU_RPC_OK
            || hako_pdu_rpc_client_start(client1_) != HAKO_PDU_RPC_OK) {
            return false;
        }
        return wait_connected(2);
    }

    void stop()
    {
        if (server_) {
            (void)hako_pdu_rpc_mux_server_stop(server_);
        }
        if (client0_) {
            (void)hako_pdu_rpc_client_stop(client0_);
        }
        if (client1_) {
            (void)hako_pdu_rpc_client_stop(client1_);
        }
        if (client0_) {
            hako_pdu_rpc_client_destroy(client0_);
            client0_ = nullptr;
        }
        if (client1_) {
            hako_pdu_rpc_client_destroy(client1_);
            client1_ = nullptr;
        }
        if (server_) {
            hako_pdu_rpc_mux_server_destroy(server_);
            server_ = nullptr;
        }
    }

    bool wait_connected(size_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        std::vector<uint8_t> scratch(kMaxPdu);
        while (std::chrono::steady_clock::now() < deadline) {
            hako_pdu_rpc_request_info_t info{};
            size_t size = 0;
            hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
            (void)hako_pdu_rpc_mux_server_poll(
                server_, &info, scratch.data(), scratch.size(), &size, &error);
            if (error != HAKO_PDU_RPC_OK) {
                return false;
            }
            if (hako_pdu_rpc_mux_server_connected_count(server_) == expected) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool call(hako_pdu_rpc_client_handle_t* client, long long a, long long b)
    {
        size_t request_size = 0;
        if (hako_pdu_rpc_client_create_request_buffer(
                client, kService, nullptr, 0, &request_size)
            != HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL) {
            return false;
        }
        std::vector<uint8_t> request(request_size);
        if (hako_pdu_rpc_client_create_request_buffer(
                client, kService, request.data(), request.size(), &request_size)
            != HAKO_PDU_RPC_OK) {
            return false;
        }
        request.resize(request_size);

        AddTwoIntsRequestPacket converter;
        HakoCpp_AddTwoIntsRequestPacket packet{};
        if (!converter.pdu2cpp(reinterpret_cast<char*>(request.data()), packet)) {
            return false;
        }
        packet.body.a = a;
        packet.body.b = b;
        const int encoded = converter.cpp2pdu(
            packet,
            reinterpret_cast<char*>(request.data()),
            static_cast<int>(request.size()));
        if (encoded <= 0) {
            return false;
        }
        request.resize(static_cast<size_t>(encoded));

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (hako_pdu_rpc_client_call(
                   client, kService, request.data(), request.size(), 1'000'000)
               != HAKO_PDU_RPC_OK) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    bool wait_request(PendingCall& call)
    {
        call.pdu.assign(kMaxPdu, 0);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            size_t size = 0;
            hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
            const auto event = hako_pdu_rpc_mux_server_poll(
                server_,
                &call.info,
                call.pdu.data(),
                call.pdu.size(),
                &size,
                &error);
            if (error != HAKO_PDU_RPC_OK) {
                return false;
            }
            if (event == HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN) {
                call.pdu.resize(size);
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool reply(const PendingCall& call, long long expected_a, long long expected_b)
    {
        AddTwoIntsRequestPacket request_converter;
        HakoCpp_AddTwoIntsRequestPacket request{};
        if (!request_converter.pdu2cpp(
                reinterpret_cast<char*>(const_cast<uint8_t*>(call.pdu.data())), request)) {
            return false;
        }
        if (request.body.a != expected_a || request.body.b != expected_b) {
            return false;
        }

        size_t reply_size = 0;
        if (hako_pdu_rpc_mux_server_create_reply_buffer(
                server_,
                call.info.request_token,
                static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE),
                static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK),
                nullptr,
                0,
                &reply_size)
            != HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL) {
            return false;
        }
        std::vector<uint8_t> reply(reply_size);
        if (hako_pdu_rpc_mux_server_create_reply_buffer(
                server_,
                call.info.request_token,
                static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE),
                static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK),
                reply.data(),
                reply.size(),
                &reply_size)
            != HAKO_PDU_RPC_OK) {
            return false;
        }
        reply.resize(reply_size);

        AddTwoIntsResponsePacket response_converter;
        HakoCpp_AddTwoIntsResponsePacket response{};
        if (!response_converter.pdu2cpp(
                reinterpret_cast<char*>(reply.data()), response)) {
            return false;
        }
        response.body.sum = request.body.a + request.body.b;
        const int encoded = response_converter.cpp2pdu(
            response,
            reinterpret_cast<char*>(reply.data()),
            static_cast<int>(reply.size()));
        if (encoded <= 0) {
            return false;
        }
        reply.resize(static_cast<size_t>(encoded));
        return hako_pdu_rpc_mux_server_send_reply(
                   server_,
                   call.info.request_token,
                   reply.data(),
                   reply.size())
            == HAKO_PDU_RPC_OK;
    }

    bool wait_response(
        hako_pdu_rpc_client_handle_t* client,
        long long expected)
    {
        std::vector<uint8_t> response(kMaxPdu);
        hako_pdu_rpc_response_info_t info{};
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            size_t size = 0;
            hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
            const auto event = hako_pdu_rpc_client_poll(
                client,
                &info,
                response.data(),
                response.size(),
                &size,
                &error);
            if (error != HAKO_PDU_RPC_OK) {
                return false;
            }
            if (event == HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN) {
                response.resize(size);
                if (std::strcmp(info.service_name, kService) != 0) {
                    return false;
                }
                AddTwoIntsResponsePacket converter;
                HakoCpp_AddTwoIntsResponsePacket packet{};
                return converter.pdu2cpp(
                           reinterpret_cast<char*>(response.data()), packet)
                    && packet.body.sum == expected;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    hako_pdu_rpc_mux_server_handle_t* server() { return server_; }
    hako_pdu_rpc_client_handle_t* client0() { return client0_; }
    hako_pdu_rpc_client_handle_t* client1() { return client1_; }

private:
    hako_pdu_rpc_mux_server_handle_t* server_{nullptr};
    hako_pdu_rpc_client_handle_t* client0_{nullptr};
    hako_pdu_rpc_client_handle_t* client1_{nullptr};
};

TEST(CRpcMuxServerContractTest, TwoClientsShareOneListener)
{
    CMuxRuntime runtime;
    ASSERT_TRUE(runtime.start());
    EXPECT_EQ(hako_pdu_rpc_mux_server_expected_count(runtime.server()), 2U);
    EXPECT_EQ(hako_pdu_rpc_mux_server_connected_count(runtime.server()), 2U);
    EXPECT_EQ(hako_pdu_rpc_mux_server_is_ready(runtime.server()), 1);

    ASSERT_TRUE(runtime.call(runtime.client0(), 10, 20));
    ASSERT_TRUE(runtime.call(runtime.client1(), 40, 2));

    std::map<std::string, PendingCall> calls;
    for (int index = 0; index < 2; ++index) {
        PendingCall call;
        ASSERT_TRUE(runtime.wait_request(call));
        calls.emplace(call.info.client_name, std::move(call));
    }
    ASSERT_TRUE(calls.contains("TestClient0"));
    ASSERT_TRUE(calls.contains("TestClient1"));

    ASSERT_TRUE(runtime.reply(calls.at("TestClient0"), 10, 20));
    ASSERT_TRUE(runtime.reply(calls.at("TestClient1"), 40, 2));
    EXPECT_TRUE(runtime.wait_response(runtime.client0(), 30));
    EXPECT_TRUE(runtime.wait_response(runtime.client1(), 42));
}

} // namespace
