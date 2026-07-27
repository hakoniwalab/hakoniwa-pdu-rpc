#include <gtest/gtest.h>

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
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

constexpr const char* kConfigPath = "configs/service_config.json";
constexpr const char* kEndpointConfigPath = "configs/endpoints.json";
constexpr const char* kServerNodeId = "server_node";
constexpr const char* kClientNodeId = "client_node";
constexpr const char* kClientName = "TestClient";
constexpr const char* kServiceName = "Service/Add";

class RpcRuntime {
public:
    RpcRuntime()
        : server_endpoint_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              kServerNodeId, kEndpointConfigPath))
        , client_endpoint_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              kClientNodeId, kEndpointConfigPath))
        , server_(kServerNodeId, "RpcServerEndpointImpl", kConfigPath, 1000)
        , client_(kClientNodeId, kClientName, kConfigPath, "RpcClientEndpointImpl", 1000)
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

bool execute_add(RpcRuntime& runtime, long long a, long long b, long long expected)
{
    HakoRpcServiceServerTemplateType(AddTwoInts) service;

    HakoCpp_AddTwoIntsRequest request_body{};
    request_body.a = a;
    request_body.b = b;
    if (!service.call(runtime.client(), kServiceName, request_body, 1'000'000)) {
        return false;
    }

    RpcRequest request;
    if (runtime.wait_server_event(request) != ServerEventType::REQUEST_IN) {
        return false;
    }

    HakoCpp_AddTwoIntsRequest parsed_request{};
    if (!service.get_request_body(request, parsed_request)) {
        return false;
    }
    if (parsed_request.a != a || parsed_request.b != b) {
        return false;
    }

    HakoCpp_AddTwoIntsResponse response_body{};
    response_body.sum = parsed_request.a + parsed_request.b;
    if (!service.reply(
            runtime.server(),
            request,
            hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
            hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
            response_body)) {
        return false;
    }

    std::string service_name;
    RpcResponse response;
    if (runtime.wait_client_event(service_name, response) != ClientEventType::RESPONSE_IN) {
        return false;
    }
    if (service_name != kServiceName) {
        return false;
    }

    HakoCpp_AddTwoIntsResponse parsed_response{};
    if (!service.get_response_body(response, parsed_response)) {
        return false;
    }
    return parsed_response.sum == expected;
}

TEST(RpcBasicContractTest, BasicRoundTrip)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());
    EXPECT_TRUE(execute_add(runtime, 5, 7, 12));
}

TEST(RpcBasicContractTest, ConsecutiveCallsReuseEndpoint)
{
    RpcRuntime runtime;
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(execute_add(runtime, 10, 20, 30));
    EXPECT_TRUE(execute_add(runtime, 15, 25, 40));
}

} // namespace
