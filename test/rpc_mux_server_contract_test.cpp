#include <gtest/gtest.h>

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_mux_server.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using hakoniwa::pdu::rpc::ClientEventType;
using hakoniwa::pdu::rpc::RpcMuxRequest;
using hakoniwa::pdu::rpc::RpcResponse;
using hakoniwa::pdu::rpc::RpcServicesClient;
using hakoniwa::pdu::rpc::RpcServicesMuxServer;
using hakoniwa::pdu::rpc::ServerEventType;

constexpr const char* kServiceConfig = "configs/service_config_mux.json";
constexpr const char* kClientEndpointConfig = "configs/endpoints_mux_clients.json";
constexpr const char* kMuxEndpointConfig = "configs/mux_server_endpoint.json";
constexpr const char* kServerNodeId = "server_node";
constexpr const char* kClientNodeId = "client_node";
constexpr const char* kServiceName = "Service/Add";

class ClientRuntime {
public:
    explicit ClientRuntime(std::string client_name)
        : client_name_(std::move(client_name))
        , endpoints_(std::make_shared<hakoniwa::pdu::EndpointContainer>(
              kClientNodeId, kClientEndpointConfig))
        , rpc_(
              kClientNodeId,
              client_name_,
              kServiceConfig,
              "RpcClientEndpointImpl",
              1000)
    {
    }

    bool start()
    {
        if (endpoints_->initialize() != HAKO_PDU_ERR_OK) {
            return false;
        }
        if (!rpc_.initialize_services(endpoints_)) {
            return false;
        }
        if (endpoints_->start_all() != HAKO_PDU_ERR_OK) {
            return false;
        }
        if (!rpc_.start_all_services()) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!endpoints_->is_running_all()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        started_ = true;
        return true;
    }

    void stop_endpoint()
    {
        if (started_) {
            (void)endpoints_->stop_all();
        }
    }

    void stop_rpc()
    {
        if (started_) {
            rpc_.stop_all_services();
            started_ = false;
        }
    }

    RpcServicesClient& rpc() { return rpc_; }
    const std::string& name() const { return client_name_; }

private:
    std::string client_name_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoints_;
    RpcServicesClient rpc_;
    bool started_{false};
};

class MuxRuntime {
public:
    MuxRuntime()
        : server_(
              kServerNodeId,
              "RpcServerEndpointImpl",
              kServiceConfig,
              kMuxEndpointConfig,
              1000)
        , client0_("TestClient0")
        , client1_("TestClient1")
    {
    }

    ~MuxRuntime()
    {
        stop();
    }

    bool start()
    {
        if (!server_.initialize() || !server_.start()) {
            return false;
        }
        if (!client0_.start() || !client1_.start()) {
            stop();
            return false;
        }
        started_ = true;
        return wait_for_connections(2);
    }

    void stop()
    {
        if (!started_) {
            server_.stop();
            return;
        }

        // Stop transport endpoints before clearing RPC instances. This also
        // interrupts any blocking TCP reads on both sides.
        server_.stop();
        client0_.stop_endpoint();
        client1_.stop_endpoint();
        client0_.stop_rpc();
        client1_.stop_rpc();
        client0_.rpc().clear_all_instances();
        started_ = false;
    }

    bool wait_for_connections(std::size_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            RpcMuxRequest ignored;
            (void)server_.poll(ignored);
            if (server_.connected_count() == expected) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    ServerEventType wait_server_event(
        RpcMuxRequest& request,
        std::chrono::milliseconds timeout = 2s)
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
        ClientRuntime& client,
        std::string& service_name,
        RpcResponse& response,
        std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto event = client.rpc().poll(service_name, response);
            if (event != ClientEventType::NONE) {
                return event;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return ClientEventType::NONE;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    RpcServicesMuxServer& server() { return server_; }
    ClientRuntime& client0() { return client0_; }
    ClientRuntime& client1() { return client1_; }

private:
    RpcServicesMuxServer server_;
    ClientRuntime client0_;
    ClientRuntime client1_;
    bool started_{false};
};

bool call_add(ClientRuntime& client, long long a, long long b)
{
    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest request{};
    request.a = a;
    request.b = b;
    return service.call(client.rpc(), kServiceName, request, 1'000'000);
}

bool reply_add(
    RpcServicesMuxServer& server,
    RpcMuxRequest& request,
    long long expected_a,
    long long expected_b)
{
    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    HakoCpp_AddTwoIntsRequest body{};
    if (!service.get_request_body(request.request, body)) {
        return false;
    }
    if (body.a != expected_a || body.b != expected_b) {
        return false;
    }

    HakoCpp_AddTwoIntsResponse response{};
    response.sum = body.a + body.b;
    return service.reply(
        server,
        request,
        hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
        hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
        response);
}

bool expect_response(MuxRuntime& runtime, ClientRuntime& client, long long expected)
{
    HakoRpcServiceServerTemplateType(AddTwoInts) service;
    std::string service_name;
    RpcResponse response;
    if (runtime.wait_client_event(client, service_name, response)
        != ClientEventType::RESPONSE_IN) {
        return false;
    }
    if (service_name != kServiceName) {
        return false;
    }
    HakoCpp_AddTwoIntsResponse body{};
    return service.get_response_body(response, body) && body.sum == expected;
}

TEST(RpcMuxServerContractTest, TwoClientsShareOneListenerAndRouteReplies)
{
    MuxRuntime runtime;
    ASSERT_TRUE(runtime.start());
    ASSERT_EQ(runtime.server().expected_count(), 2U);
    ASSERT_EQ(runtime.server().connected_count(), 2U);
    ASSERT_TRUE(runtime.server().is_ready());

    ASSERT_TRUE(call_add(runtime.client0(), 10, 20));
    ASSERT_TRUE(call_add(runtime.client1(), 40, 2));

    std::map<std::string, RpcMuxRequest> requests;
    for (int index = 0; index < 2; ++index) {
        RpcMuxRequest request;
        ASSERT_EQ(runtime.wait_server_event(request), ServerEventType::REQUEST_IN);
        requests.emplace(request.request.header.client_name, std::move(request));
    }

    ASSERT_EQ(requests.size(), 2U);
    ASSERT_TRUE(requests.contains("TestClient0"));
    ASSERT_TRUE(requests.contains("TestClient1"));

    const std::set<std::uint64_t> connection_ids = {
        requests.at("TestClient0").connection_id,
        requests.at("TestClient1").connection_id,
    };
    EXPECT_EQ(connection_ids.size(), 2U);

    ASSERT_TRUE(reply_add(runtime.server(), requests.at("TestClient0"), 10, 20));
    ASSERT_TRUE(reply_add(runtime.server(), requests.at("TestClient1"), 40, 2));

    EXPECT_TRUE(expect_response(runtime, runtime.client0(), 30));
    EXPECT_TRUE(expect_response(runtime, runtime.client1(), 42));
}

TEST(RpcMuxServerContractTest, ReusesAcceptedClientSessionForSequentialCalls)
{
    MuxRuntime runtime;
    ASSERT_TRUE(runtime.start());

    std::uint64_t first_connection = 0;
    for (const auto& values : {std::pair{5LL, 7LL}, std::pair{11LL, 13LL}}) {
        ASSERT_TRUE(call_add(runtime.client0(), values.first, values.second));
        RpcMuxRequest request;
        ASSERT_EQ(runtime.wait_server_event(request), ServerEventType::REQUEST_IN);
        ASSERT_EQ(request.request.header.client_name, "TestClient0");
        if (first_connection == 0) {
            first_connection = request.connection_id;
        } else {
            EXPECT_EQ(request.connection_id, first_connection);
        }
        ASSERT_TRUE(reply_add(
            runtime.server(), request, values.first, values.second));
        ASSERT_TRUE(expect_response(
            runtime, runtime.client0(), values.first + values.second));
    }
}

} // namespace
