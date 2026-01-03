#include <gtest/gtest.h>
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h> // For chdir, getcwd
#include <cstdlib>  // For free

// Ensure the working directory is set to the project root for tests
class DirectoryChanger {
public:
    DirectoryChanger(const std::string& target_dir) {
        char* current_dir = getcwd(NULL, 0);
        if (current_dir) {
            original_dir_ = current_dir;
        } else {
            original_dir_ = strdup(""); // Fallback in case getcwd fails
            std::cerr << "WARNING: Failed to get current working directory." << std::endl;
        }

        if (chdir(target_dir.c_str()) != 0) {
            std::cerr << "ERROR: Failed to change directory to " << target_dir << std::endl;
            // Handle error appropriately, maybe throw exception
            throw std::runtime_error("Failed to change directory.");
        }
    }

    ~DirectoryChanger() {
        if (chdir(original_dir_) != 0) {
            std::cerr << "ERROR: Failed to restore directory to " << original_dir_ << std::endl;
        }
        free(original_dir_);
    }

private:
    char* original_dir_;
};

// Test fixture for RPC services
class RpcServicesTest : public ::testing::Test {
protected:
    // Path to the unified service config file
    const std::string config_path_ = "configs/service_config.json";
    // Node IDs from the config
    const std::string server_node_id_ = "server_node";
    const std::string client_node_id_ = "client_node";
    // Client name for the RpcServicesClient instance
    const std::string rpc_client_instance_name_ = "TestClient";
    // Service name used in config
    const std::string service_name_ = "Service/Add";

    // Static object to change directory once for all tests in this fixture
    static DirectoryChanger* dir_changer_;

    static void SetUpTestSuite() {
        dir_changer_ = new DirectoryChanger("/Users/tmori/project/oss/work/hakoniwa-pdu-rpc/test");
    }

    static void TearDownTestSuite() {
        delete dir_changer_;
        dir_changer_ = nullptr;
    }

    // Set up objects for each test
    void SetUp() override {
        // Individual test setup if needed
    }

    // Clean up objects after each test
    void TearDown() override {
        // Individual test cleanup if necessary
    }
};

// Initialize static member
DirectoryChanger* RpcServicesTest::dir_changer_ = nullptr;

// Test case for configuration parsing
TEST_F(RpcServicesTest, ConfigParsingTest) {
    // Initialize server
    hakoniwa::pdu::rpc::RpcServicesServer server(server_node_id_, "PduRpcServerEndpointImpl", config_path_, 1000);
    ASSERT_TRUE(server.initialize_services());

    // Initialize client
    //hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, config_path_, "PduRpcClientEndpointImpl", 1000);
    //ASSERT_TRUE(client.initialize_services());

    // Further assertions can be added here to check specific details of the initialized services
    // For example, checking if the expected service names are registered.
    // This might require adding getter methods to RpcServicesServer/Client
}