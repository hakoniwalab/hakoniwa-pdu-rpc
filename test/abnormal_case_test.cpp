#include <gtest/gtest.h>
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_types.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <cstdlib>

using hakoniwa::pdu::rpc::PduData;
class DirectoryChanger {
public:
    DirectoryChanger(const std::string& target_dir) {
        char* current_dir = getcwd(NULL, 0);
        if (current_dir) {
            original_dir_ = current_dir;
            free(current_dir);
        } else {
            original_dir_ = "";
            std::cerr << "WARNING: Failed to get current working directory." << std::endl;
        }

        if (chdir(target_dir.c_str()) != 0) {
            std::cerr << "ERROR: Failed to change directory to " << target_dir << std::endl;
            throw std::runtime_error("Failed to change directory.");
        }
    }

    ~DirectoryChanger() {
        if (!original_dir_.empty() && chdir(original_dir_.c_str()) != 0) {
            std::cerr << "ERROR: Failed to restore directory to " << original_dir_ << std::endl;
        }
    }

private:
    std::string original_dir_;
};
// Test fixture for abnormal cases in RPC services
class AbnormalCaseTest : public ::testing::Test {
protected:
    static DirectoryChanger* dir_changer_;
    // Node IDs can be consistent across tests
    const std::string server_node_id_ = "server_node";
    const std::string client_node_id_ = "client_node";
    const std::string rpc_client_instance_name_ = "TestClient";
    const std::string config_dir_ = "tmp_configs";
    static void SetUpTestSuite() {
        dir_changer_ = new DirectoryChanger("../../test");
    }

    static void TearDownTestSuite() {
        delete dir_changer_;
        dir_changer_ = nullptr;
    }

    void SetUp() override {
        // Create a temporary directory for test configs
        std::string command = "mkdir -p " + config_dir_;
        system(command.c_str());
    }

    void TearDown() override {
        // Clean up the temporary directory
        std::string command = "rm -rf " + config_dir_;
        system(command.c_str());
    }

    // Helper to create a dummy config file
    void CreateConfigFile(const std::string& path, const std::string& content) {
        std::ofstream ofs(path);
        ASSERT_TRUE(ofs.is_open());
        ofs << content;
        ofs.close();
    }
};
DirectoryChanger* AbnormalCaseTest::dir_changer_ = nullptr;


TEST_F(AbnormalCaseTest, MissingConfigFile) {
    hakoniwa::pdu::rpc::RpcServicesServer server(server_node_id_, "RpcServerEndpointImpl", "non_existent_config.json", 1000);
    ASSERT_FALSE(server.initialize_services());

    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, "non_existent_config.json", "RpcClientEndpointImpl", 1000);
    ASSERT_FALSE(client.initialize_services());
}

TEST_F(AbnormalCaseTest, MalformedJsonConfigFile) {
    const std::string malformed_config_path = config_dir_ + "/malformed.json";
    CreateConfigFile(malformed_config_path, "{ \"services\": [ }"); // Invalid JSON

    hakoniwa::pdu::rpc::RpcServicesServer server(server_node_id_, "RpcServerEndpointImpl", malformed_config_path, 1000);
    ASSERT_FALSE(server.initialize_services());

    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, malformed_config_path, "RpcClientEndpointImpl", 1000);
    ASSERT_FALSE(client.initialize_services());
}

TEST_F(AbnormalCaseTest, MissingEndpointsSection) {
    const std::string config_path = config_dir_ + "/missing_endpoints.json";
    CreateConfigFile(config_path, R"({
        "services": [{
            "name": "TestService",
            "server_endpoint": {"nodeId": "server_node", "endpointId": "ep1"},
            "clients": [{"name": "TestClient", "client_endpoint": {"nodeId": "client_node", "endpointId": "ep2"}}]
        }]
    })");

    hakoniwa::pdu::rpc::RpcServicesServer server(server_node_id_, "RpcServerEndpointImpl", config_path, 1000);
    ASSERT_FALSE(server.initialize_services()); // Fails because endpoint configs are missing

    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, config_path, "RpcClientEndpointImpl", 1000);
    ASSERT_FALSE(client.initialize_services()); // Fails for the same reason
}

TEST_F(AbnormalCaseTest, ClientServiceDefinitionMissing) {
    const std::string config_path = config_dir_ + "/client_missing.json";
    CreateConfigFile(config_path, R"({
        "endpoints": [
            {"nodeId": "client_node", "endpoints": [{"id": "ep1", "config_path": "dummy_path"}]}
        ],
        "services": [{
            "name": "TestService",
            "server_endpoints": [{"nodeId": "server_node", "endpointId": "ep1"}],
            "clients": [{"name": "AnotherClient", "client_endpoint": {"nodeId": "client_node", "endpointId": "ep1"}}]
        }]
    })");

    // Client initialization should still "succeed" but initialize zero services.
    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, config_path, "RpcClientEndpointImpl", 1000);
    ASSERT_TRUE(client.initialize_services());

    // Calling a service should fail because it was not initialized for this client
    PduData pdu;
    ASSERT_FALSE(client.call("TestService", pdu, 1000));
}

TEST_F(AbnormalCaseTest, CallNonExistentService) {
    // Use a valid config but call a service not defined in it
    const std::string config_path = "configs/service_config.json"; // Assuming this is a valid file from the original tests
    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, config_path, "RpcClientEndpointImpl", 1000);
    ASSERT_TRUE(client.initialize_services());

    PduData pdu;
    ASSERT_FALSE(client.call("NonExistentService", pdu, 1000));
}

TEST_F(AbnormalCaseTest, StartServiceFails) {
    const std::string ep_config_path = config_dir_ + "/invalid_ep_config.json";
    CreateConfigFile(ep_config_path, R"({"type": "invalid_type"})"); // Invalid endpoint config

    const std::string service_config_path = config_dir_ + "/service_with_bad_ep.json";
    CreateConfigFile(service_config_path, R"({
        "endpoints": [
            {"nodeId": "server_node", "endpoints": [{"id": "ep1", "config_path": ")" + ep_config_path + R"("}]}
        ],
        "services": [{
            "name": "TestService",
            "server_endpoint": {"nodeId": "server_node", "endpointId": "ep1"},
            "clients": []
        }]
    })");

    hakoniwa::pdu::rpc::RpcServicesServer server(server_node_id_, "RpcServerEndpointImpl", service_config_path, 1000);
    // Initialization might succeed, but starting will fail because endpoint `open` fails.
    // Let's adjust based on the finding that open is part of `initialize_services`.
    ASSERT_FALSE(server.initialize_services()); 
}


TEST_F(AbnormalCaseTest, CreateBufferForNonExistentService) {
    const std::string config_path = "configs/service_config.json";
    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, config_path, "RpcClientEndpointImpl", 1000);
    ASSERT_TRUE(client.initialize_services());

    PduData pdu;
    ASSERT_FALSE(client.create_request_buffer("NonExistentService", pdu));
    ASSERT_FALSE(client.create_request_buffer("NonExistentService", 0, pdu));
}
