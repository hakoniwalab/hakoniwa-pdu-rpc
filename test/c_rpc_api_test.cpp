#include <gtest/gtest.h>

#include "hakoniwa/pdu/rpc/c_rpc.h"

namespace {
constexpr const char* kServiceConfigPath = "configs/service_config.json";
constexpr const char* kEndpointConfigPath = "configs/endpoints.json";
}

TEST(CRpcApiTest, RejectsInvalidCreateArguments) {
    EXPECT_EQ(hako_pdu_rpc_client_create(nullptr, "client", "service.json", "endpoint.json", 1000, "real"), nullptr);
    EXPECT_EQ(hako_pdu_rpc_server_create(nullptr, "service.json", "endpoint.json", 1000, "real"), nullptr);
}

TEST(CRpcApiTest, InitializedHandleCanBeDestroyedWithoutStarting) {
    auto* server = hako_pdu_rpc_server_create(
        "server_node", kServiceConfigPath, kEndpointConfigPath, 1000, "real");
    ASSERT_NE(server, nullptr);

    auto* client = hako_pdu_rpc_client_create(
        "client_node", "TestClient", kServiceConfigPath, kEndpointConfigPath, 1000, "real");
    ASSERT_NE(client, nullptr);

    hako_pdu_rpc_client_destroy(client);
    hako_pdu_rpc_server_destroy(server);
}

TEST(CRpcApiTest, NullLifecycleCallsAreSafe) {
    EXPECT_EQ(hako_pdu_rpc_client_start(nullptr), HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(hako_pdu_rpc_client_stop(nullptr), HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(hako_pdu_rpc_server_start(nullptr), HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(hako_pdu_rpc_server_stop(nullptr), HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT);

    hako_pdu_rpc_client_destroy(nullptr);
    hako_pdu_rpc_server_destroy(nullptr);
}
