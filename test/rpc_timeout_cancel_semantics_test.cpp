#include <gtest/gtest.h>

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using hakoniwa::pdu::rpc::ClientEventType;
using hakoniwa::pdu::rpc::RpcRequest;
using hakoniwa::pdu::rpc::RpcResponse;
using hakoniwa::pdu::rpc::RpcServicesClient;
using hakoniwa::pdu::rpc::RpcServicesServer;
using hakoniwa::pdu::rpc::ServerEventType;

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& target)
        : original_(std::filesystem::current_path())
    {
        std::filesystem::current_path(target);
    }

    ~ScopedCurrentPath()
    {
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }

private:
    std::filesystem::path original_;
};

class RpcTimeoutCancelSemanticsTest : public ::testing::Test {
public:
    static void SetUpTestSuite()
    {
        cwd_ = std::make_unique<ScopedCurrentPath>("../../test");
    }

    static void TearDownTestSuite()
    {
        cwd_.reset();
    }

    static inline std::unique_ptr<ScopedCurrentPath> cwd_;

    static constexpr const char* kConfigPath = "configs/service_config.json";
    static constexpr const char* kEndpointConfigPath = "configs/endpoints.json";
    static constexpr const char* kServerNodeId = "server_node";
    static constexpr const char* kClientNodeId = "client_node";
    static constexpr const char* kClientName = "TestClient";
    static constexpr const char* kServiceName = "Service/Add";
};

class RpcRuntime {
public:
    RpcRuntime()
        : server_endpoint_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              RpcTimeoutCancelSemanticsTest::kServerNodeId,
              RpcTimeoutCancelSemanticsTest::kEndpointConfigPath))
        , client_endpoint_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              RpcTimeoutCancelSemanticsTest::kClientNodeId,
              RpcTimeoutCancelSemanticsTest::kEndpointConfigPath))
        , server_(
              RpcTimeoutCancelSemanticsTest::kServerNodeId,
              "RpcServerEndpointImpl",
              RpcTimeoutCancelSemanticsTest::kConfigPath,
              1000)
        , client_(
              RpcTimeoutCancelSemanticsTest::kClientNodeId,
              RpcTimeoutCancelSemanticsTest::kClientName,
              RpcTimeoutCancelSemanticsTest::kConfigPath,
              "RpcClientEndpointImpl",
              1000)
    {
    }

    ~RpcRuntime()
    {
        stop();
    }

    bool start()
    {
        if (server_endpoint_->initialize() != HAKO_PDU_ERR_OK ||
            client_endpoint_->initialize() != HAKO_PDU_ERR_OK) {
            return false;
        }
        if (!server_.initialize_services(server_endpoint_) ||
            !client_.initialize_services(client_endpoint_)) {
            return false;
        }
        if (server_endpoint_->start_all() != HAKO_PDU_ERR_OK ||
            client_endpoint_->start_all() != HAKO_PDU_ERR_OK) {
            return false;
        }
        if (!server_.start_all_services() || !client_.start_all_services()) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!server_endpoint_->is_running_all() || !client_endpoint_->is_running_all()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        started_ = true;
        return true;
    }

    void stop()
    {
        if (!started_) {
            return;
        }
        server_endpoint_->stop_all();
        client_endpoint_->stop_all();
        server_.stop_all_services();
        client_.stop_all_services();
        server_.clear_all_instances();
        client_.clear_all_instances();
        started_ = false;
    }

    ServerEventType wait_server_event(RpcRequest& request, std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto event = server_.poll(request);
            if (event != ServerEventType::NONE) {
                return event;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return ServerEventType::NONE;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    ClientEventType wait_client_event(
        std::string& service_name,
        RpcResponse& response,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto event = client_.poll(service_name, response);
            if (event != ClientEventType::NONE) {
                return event;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return ClientEventType::NONE;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    RpcServicesServer& server() { return server_; }
    RpcServicesClient& client() { return client_; }

private:
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> server_endpoint_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> client_endpoint_;
    RpcServicesServer server_;
    RpcServicesClient client_;
    bool started_ = false;
};

TEST_F(RpcTimeoutCancelSemanticsTest, TimeoutRequiresExplicitCancelAndReturnsToIdle)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());

    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest request_body{};
    request_body.a = 10;
    request_body.b = 20;
    ASSERT_TRUE(service.call(runtime.client(), kServiceName, request_body, 50'000));

    RpcRequest original_request;
    ASSERT_EQ(runtime.wait_server_event(original_request), ServerEventType::REQUEST_IN);

    std::string service_name;
    RpcResponse response;
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_TIMEOUT);
    ASSERT_EQ(service_name, kServiceName);

    // Core-compatible contract: timeout notification itself does not cancel.
    // The caller explicitly sends cancellation for the still-running request.
    ASSERT_TRUE(runtime.client().send_cancel_request(kServiceName));

    RpcRequest cancel_request;
    ASSERT_EQ(runtime.wait_server_event(cancel_request), ServerEventType::REQUEST_CANCEL);
    ASSERT_EQ(cancel_request.header.request_id, original_request.header.request_id);

    hakoniwa::pdu::rpc::PduData cancel_response;
    runtime.server().create_reply_buffer(
        cancel_request.header,
        hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_CANCELED,
        cancel_response);
    runtime.server().send_cancel_reply(cancel_request.header, cancel_response);

    service_name.clear();
    response = {};
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_CANCEL);
    ASSERT_EQ(service_name, kServiceName);

    // After cancel completion the same RPC endpoint must be reusable.
    HakoCpp_AddTwoIntsRequest second_request_body{};
    second_request_body.a = 7;
    second_request_body.b = 8;
    ASSERT_TRUE(service.call(runtime.client(), kServiceName, second_request_body, 1'000'000));

    RpcRequest second_request;
    ASSERT_EQ(runtime.wait_server_event(second_request), ServerEventType::REQUEST_IN);
    HakoCpp_AddTwoIntsResponse second_response_body{};
    second_response_body.sum = 15;
    ASSERT_TRUE(service.reply(
        runtime.server(),
        second_request,
        hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
        second_response_body));

    service_name.clear();
    response = {};
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_IN);
}

TEST_F(RpcTimeoutCancelSemanticsTest, NormalResponseMayWinRaceAfterTimeout)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());

    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest request_body{};
    request_body.a = 3;
    request_body.b = 4;
    ASSERT_TRUE(service.call(runtime.client(), kServiceName, request_body, 50'000));

    RpcRequest server_request;
    ASSERT_EQ(runtime.wait_server_event(server_request), ServerEventType::REQUEST_IN);

    std::string service_name;
    RpcResponse response;
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_TIMEOUT);

    // Match the race covered by hakoniwa-core-pro: the server may finish after
    // the client observed timeout but before cancellation is issued.
    HakoCpp_AddTwoIntsResponse response_body{};
    response_body.sum = 7;
    ASSERT_TRUE(service.reply(
        runtime.server(),
        server_request,
        hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
        response_body));

    service_name.clear();
    response = {};
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_IN);
    ASSERT_EQ(service_name, kServiceName);

    HakoCpp_AddTwoIntsResponse parsed_response{};
    ASSERT_TRUE(service.get_response_body(response, parsed_response));
    EXPECT_EQ(parsed_response.sum, 7);

    // The normal response completed the request, so a late cancellation is no
    // longer valid. A new request must nevertheless start normally.
    EXPECT_FALSE(runtime.client().send_cancel_request(kServiceName));

    HakoCpp_AddTwoIntsRequest next_request{};
    next_request.a = 1;
    next_request.b = 2;
    EXPECT_TRUE(service.call(runtime.client(), kServiceName, next_request, 1'000'000));
}

} // namespace
