#include <gtest/gtest.h>

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstdint>
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

class RpcServicesTest : public ::testing::Test {
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
              RpcServicesTest::kServerNodeId,
              RpcServicesTest::kEndpointConfigPath))
        , client_endpoint_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              RpcServicesTest::kClientNodeId,
              RpcServicesTest::kEndpointConfigPath))
        , server_(
              RpcServicesTest::kServerNodeId,
              "RpcServerEndpointImpl",
              RpcServicesTest::kConfigPath,
              1000)
        , client_(
              RpcServicesTest::kClientNodeId,
              RpcServicesTest::kClientName,
              RpcServicesTest::kConfigPath,
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
        server_.stop_all_services();
        client_.stop_all_services();
        server_.clear_all_instances();
        client_.clear_all_instances();
        server_endpoint_->stop_all();
        client_endpoint_->stop_all();
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

TEST_F(RpcServicesTest, ConfigParsingTest)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());

    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest request{};
    request.a = 5;
    request.b = 7;
    ASSERT_TRUE(service.call(runtime.client(), kServiceName, request, 1'000'000));

    RpcRequest server_request;
    ASSERT_EQ(runtime.wait_server_event(server_request), ServerEventType::REQUEST_IN);

    HakoCpp_AddTwoIntsRequest parsed_request{};
    ASSERT_TRUE(service.get_request_body(server_request, parsed_request));
    EXPECT_EQ(parsed_request.a, 5);
    EXPECT_EQ(parsed_request.b, 7);

    HakoCpp_AddTwoIntsResponse response_body{};
    response_body.sum = parsed_request.a + parsed_request.b;
    ASSERT_TRUE(service.reply(
        runtime.server(),
        server_request,
        hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
        response_body));

    std::string service_name;
    RpcResponse response;
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_IN);
    EXPECT_EQ(service_name, kServiceName);

    HakoCpp_AddTwoIntsResponse parsed_response{};
    ASSERT_TRUE(service.get_response_body(response, parsed_response));
    EXPECT_EQ(parsed_response.sum, 12);
}

TEST_F(RpcServicesTest, RpcCallTimeoutTest)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());

    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest request{};
    request.a = 5;
    request.b = 7;
    ASSERT_TRUE(service.call(runtime.client(), kServiceName, request, 100'000));

    RpcRequest original_request;
    ASSERT_EQ(runtime.wait_server_event(original_request), ServerEventType::REQUEST_IN);

    std::string service_name;
    RpcResponse response;
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_TIMEOUT);
    EXPECT_EQ(service_name, kServiceName);

    // Core-compatible contract: timeout is only a notification. Resolve the
    // still-running RPC explicitly before tearing down the transport.
    ASSERT_TRUE(runtime.client().send_cancel_request(kServiceName));

    RpcRequest cancel_request;
    ASSERT_EQ(runtime.wait_server_event(cancel_request), ServerEventType::REQUEST_CANCEL);
    EXPECT_EQ(cancel_request.header.request_id, original_request.header.request_id);

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
    EXPECT_EQ(service_name, kServiceName);
}

TEST_F(RpcServicesTest, MultipleServiceCallsTest)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());

    HakoRpcServiceServerTemplateType(AddTwoInts) service;

    const auto round_trip = [&](std::int64_t a, std::int64_t b, std::int64_t expected) {
        HakoCpp_AddTwoIntsRequest request{};
        request.a = a;
        request.b = b;
        ASSERT_TRUE(service.call(runtime.client(), kServiceName, request, 1'000'000));

        RpcRequest server_request;
        ASSERT_EQ(runtime.wait_server_event(server_request), ServerEventType::REQUEST_IN);

        HakoCpp_AddTwoIntsRequest parsed_request{};
        ASSERT_TRUE(service.get_request_body(server_request, parsed_request));
        EXPECT_EQ(parsed_request.a, a);
        EXPECT_EQ(parsed_request.b, b);

        HakoCpp_AddTwoIntsResponse response_body{};
        response_body.sum = parsed_request.a + parsed_request.b;
        ASSERT_TRUE(service.reply(
            runtime.server(),
            server_request,
            hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
            hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
            response_body));

        std::string service_name;
        RpcResponse response;
        ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_IN);
        EXPECT_EQ(service_name, kServiceName);

        HakoCpp_AddTwoIntsResponse parsed_response{};
        ASSERT_TRUE(service.get_response_body(response, parsed_response));
        EXPECT_EQ(parsed_response.sum, expected);
    };

    round_trip(10, 20, 30);
    round_trip(15, 25, 40);
}

TEST_F(RpcServicesTest, RpcCallInfiniteWaitTest)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());

    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest request{};
    request.a = 99;
    request.b = 1;
    ASSERT_TRUE(service.call(runtime.client(), kServiceName, request, 0));

    RpcRequest server_request;
    ASSERT_EQ(runtime.wait_server_event(server_request), ServerEventType::REQUEST_IN);

    std::this_thread::sleep_for(200ms);

    HakoCpp_AddTwoIntsRequest parsed_request{};
    ASSERT_TRUE(service.get_request_body(server_request, parsed_request));
    EXPECT_EQ(parsed_request.a, 99);
    EXPECT_EQ(parsed_request.b, 1);

    HakoCpp_AddTwoIntsResponse response_body{};
    response_body.sum = parsed_request.a + parsed_request.b;
    ASSERT_TRUE(service.reply(
        runtime.server(),
        server_request,
        hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
        response_body));

    std::string service_name;
    RpcResponse response;
    ASSERT_EQ(runtime.wait_client_event(service_name, response), ClientEventType::RESPONSE_IN);
    EXPECT_EQ(service_name, kServiceName);

    HakoCpp_AddTwoIntsResponse parsed_response{};
    ASSERT_TRUE(service.get_response_body(response, parsed_response));
    EXPECT_EQ(parsed_response.sum, 100);
}

} // namespace
