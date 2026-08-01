#include <gtest/gtest.h>

#include "hakoniwa/pdu/rpc/c_rpc.h"
#include "hakoniwa/pdu/rpc/rpc_types.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
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

class CRpcRuntime {
public:
    ~CRpcRuntime()
    {
        if (server_ && client_) (void)hako_pdu_rpc_stop_pair(server_, client_);
        if (client_) hako_pdu_rpc_client_destroy(client_);
        if (server_) hako_pdu_rpc_server_destroy(server_);
    }

    bool start()
    {
        server_ = hako_pdu_rpc_server_create("server_node", kServiceConfig, kEndpointConfig, 1000, "real");
        client_ = hako_pdu_rpc_client_create("client_node", "TestClient", kServiceConfig, kEndpointConfig, 1000, "real");
        return server_ && client_ &&
            hako_pdu_rpc_server_start(server_) == HAKO_PDU_RPC_OK &&
            hako_pdu_rpc_client_start(client_) == HAKO_PDU_RPC_OK;
    }

    bool execute(long long a, long long b, long long expected)
    {
        size_t request_size = 0;
        if (hako_pdu_rpc_client_create_request_buffer(client_, kService, nullptr, 0, &request_size) != HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL) return false;
        std::vector<uint8_t> request(request_size);
        if (hako_pdu_rpc_client_create_request_buffer(client_, kService, request.data(), request.size(), &request_size) != HAKO_PDU_RPC_OK) return false;
        request.resize(request_size);

        AddTwoIntsRequestPacket request_converter;
        HakoCpp_AddTwoIntsRequestPacket request_cpp{};
        if (!request_converter.pdu2cpp(reinterpret_cast<char*>(request.data()), request_cpp)) return false;
        request_cpp.body.a = a;
        request_cpp.body.b = b;
        const int encoded_request = request_converter.cpp2pdu(request_cpp, reinterpret_cast<char*>(request.data()), static_cast<int>(request.size()));
        if (encoded_request <= 0) return false;
        request.resize(static_cast<size_t>(encoded_request));

        const auto connect_deadline = std::chrono::steady_clock::now() + 3s;
        while (hako_pdu_rpc_client_call(client_, kService, request.data(), request.size(), 1'000'000) != HAKO_PDU_RPC_OK) {
            if (std::chrono::steady_clock::now() >= connect_deadline) return false;
            std::this_thread::sleep_for(10ms);
        }

        hako_pdu_rpc_request_info_t request_info{};
        std::vector<uint8_t> received_request(kMaxPdu);
        if (wait_server(request_info, received_request) != HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN) return false;
        if (std::strcmp(request_info.service_name, kService) != 0) return false;

        HakoCpp_AddTwoIntsRequestPacket received_cpp{};
        if (!request_converter.pdu2cpp(reinterpret_cast<char*>(received_request.data()), received_cpp)) return false;
        if (received_cpp.body.a != a || received_cpp.body.b != b) return false;

        size_t reply_size = 0;
        if (hako_pdu_rpc_server_create_reply_buffer(server_, request_info.request_token,
                static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE),
                static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK),
                nullptr, 0, &reply_size) != HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL) return false;
        std::vector<uint8_t> reply(reply_size);
        if (hako_pdu_rpc_server_create_reply_buffer(server_, request_info.request_token,
                static_cast<uint8_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE),
                static_cast<int32_t>(hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK),
                reply.data(), reply.size(), &reply_size) != HAKO_PDU_RPC_OK) return false;
        reply.resize(reply_size);

        AddTwoIntsResponsePacket response_converter;
        HakoCpp_AddTwoIntsResponsePacket response_cpp{};
        if (!response_converter.pdu2cpp(reinterpret_cast<char*>(reply.data()), response_cpp)) return false;
        response_cpp.body.sum = a + b;
        const int encoded_reply = response_converter.cpp2pdu(response_cpp, reinterpret_cast<char*>(reply.data()), static_cast<int>(reply.size()));
        if (encoded_reply <= 0) return false;
        reply.resize(static_cast<size_t>(encoded_reply));
        if (hako_pdu_rpc_server_send_reply(server_, request_info.request_token, reply.data(), reply.size()) != HAKO_PDU_RPC_OK) return false;

        hako_pdu_rpc_response_info_t response_info{};
        std::vector<uint8_t> response(kMaxPdu);
        if (wait_client(response_info, response) != HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN) return false;
        if (std::strcmp(response_info.service_name, kService) != 0) return false;
        HakoCpp_AddTwoIntsResponsePacket parsed{};
        return response_converter.pdu2cpp(reinterpret_cast<char*>(response.data()), parsed) && parsed.body.sum == expected;
    }

private:
    hako_pdu_rpc_server_event_t wait_server(hako_pdu_rpc_request_info_t& info, std::vector<uint8_t>& pdu)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            size_t size = 0;
            hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
            const auto event = hako_pdu_rpc_server_poll(server_, &info, pdu.data(), pdu.size(), &size, &error);
            if (error != HAKO_PDU_RPC_OK) return HAKO_PDU_RPC_SERVER_EVENT_NONE;
            if (event != HAKO_PDU_RPC_SERVER_EVENT_NONE) { pdu.resize(size); return event; }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }

    hako_pdu_rpc_client_event_t wait_client(hako_pdu_rpc_response_info_t& info, std::vector<uint8_t>& pdu)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            size_t size = 0;
            hako_pdu_rpc_error_t error = HAKO_PDU_RPC_OK;
            const auto event = hako_pdu_rpc_client_poll(client_, &info, pdu.data(), pdu.size(), &size, &error);
            if (error != HAKO_PDU_RPC_OK) return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
            if (event != HAKO_PDU_RPC_CLIENT_EVENT_NONE) { pdu.resize(size); return event; }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }

    hako_pdu_rpc_server_handle_t* server_{nullptr};
    hako_pdu_rpc_client_handle_t* client_{nullptr};
};

TEST(CRpcBasicContractTest, BasicRoundTrip)
{
    CRpcRuntime runtime;
    ASSERT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.execute(5, 7, 12));
}

TEST(CRpcBasicContractTest, ConsecutiveCallsReuseEndpoint)
{
    CRpcRuntime runtime;
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.execute(10, 20, 30));
    EXPECT_TRUE(runtime.execute(15, 25, 40));
}

} // namespace
