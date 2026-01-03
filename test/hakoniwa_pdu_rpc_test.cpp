#include <gtest/gtest.h>
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"
#include "pdu_convertor.hpp"
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
    hakoniwa::pdu::rpc::RpcServicesClient client(client_node_id_, rpc_client_instance_name_, config_path_, "PduRpcClientEndpointImpl", 1000);
    ASSERT_TRUE(client.initialize_services());

    server.start_all_services();
    client.start_all_services();

    while (!server.is_pdu_end_point_running() || !client.is_pdu_end_point_running()) {
        usleep(1000); // Wait for 1ms before checking again
    }
    HakoRpcServiceServerTemplateType(AddTwoInts) service_helper;

    // Client side: send request
    {
        HakoCpp_AddTwoIntsRequest client_req_body;
        client_req_body.a = 5;
        client_req_body.b = 7;
        ASSERT_TRUE(service_helper.call(client, service_name_, client_req_body, 1000000)); // 1 second timeout
    }

    // Server side: Poll for request
    {
        hakoniwa::pdu::rpc::RpcRequest server_request;
        hakoniwa::pdu::rpc::ServerEventType server_event = hakoniwa::pdu::rpc::ServerEventType::NONE;
        while (server_event == hakoniwa::pdu::rpc::ServerEventType::NONE) {
            usleep(1000); // Wait for 1ms before polling again
            server_event = server.poll(server_request);
        }
        ASSERT_EQ(server_event, hakoniwa::pdu::rpc::ServerEventType::REQUEST_IN);

        // get HakoCpp_AddTwoIntsRequest from packet
        HakoCpp_AddTwoIntsRequest req_body;
        bool got_req_body = service_helper.get_request_body(server_request, req_body);
        ASSERT_TRUE(got_req_body);
        ASSERT_EQ(req_body.a, 5);
        ASSERT_EQ(req_body.b, 7);

        // set reply data
        HakoCpp_AddTwoIntsResponse res_body;
        res_body.sum = req_body.a + req_body.b;

        // get pdu data for response buffer
        ASSERT_TRUE(service_helper.reply(server, server_request, 
            hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE, 
            hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK, 
            res_body));
    }

    // Client side: Poll for response
    {
        hakoniwa::pdu::rpc::RpcResponse client_response;
        hakoniwa::pdu::rpc::ClientEventType client_event = hakoniwa::pdu::rpc::ClientEventType::NONE;
        std::string srvname;
        while (client_event == hakoniwa::pdu::rpc::ClientEventType::NONE) {
            usleep(1000); // Wait for 1ms before polling again
            client_event = client.poll(srvname, client_response);
        }
        ASSERT_EQ(client_event, hakoniwa::pdu::rpc::ClientEventType::RESPONSE_IN);
        ASSERT_EQ(srvname, service_name_);

        HakoCpp_AddTwoIntsResponse client_res_body;
        bool got_res_body = service_helper.get_response_body(client_response, client_res_body);
        ASSERT_TRUE(got_res_body);
        ASSERT_EQ(client_res_body.sum, 12);
    }
}