#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {
long long parse_timeout(const char* value) {
    try {
        return std::stoll(value);
    } catch (...) {
        std::cerr << "Invalid timeout_usec: " << value << std::endl;
        std::exit(1);
    }
}

bool parse_two_i64(const std::string& line, long long& a, long long& b) {
    std::istringstream iss(line);
    if (!(iss >> a >> b)) {
        return false;
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [timeout_usec]" << std::endl;
        return 1;
    }
    const long long timeout_usec = (argc == 2) ? parse_timeout(argv[1]) : 1000000;

    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        "client_node", "config/sample/endpoints.json");
    if (endpoints->initialize() != HAKO_PDU_ERR_OK) {
        std::cerr << "Failed to initialize endpoints" << std::endl;
        return 1;
    }
    if (endpoints->start_all() != HAKO_PDU_ERR_OK) {
        std::cerr << "Failed to start endpoints" << std::endl;
        return 1;
    }

    hakoniwa::pdu::rpc::RpcServicesClient client(
        "client_node", "TestClient", "config/sample/simple-service.json", "RpcClientEndpointImpl", 1000);
    if (!client.initialize_services(endpoints)) {
        std::cerr << "Failed to initialize RPC services" << std::endl;
        return 1;
    }
    client.start_all_services();

    HakoRpcServiceServerTemplateType(AddTwoInts) helper;

    std::cout << "Enter two integers per line (or 'q' to quit):" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "quit") {
            break;
        }
        long long a = 0;
        long long b = 0;
        if (!parse_two_i64(line, a, b)) {
            std::cerr << "Invalid input. Expected: <a> <b>" << std::endl;
            continue;
        }

        HakoCpp_AddTwoIntsRequest req;
        req.a = a;
        req.b = b;
        if (!helper.call(client, "Service/Add", req, static_cast<uint64_t>(timeout_usec))) {
            std::cerr << "Failed to send RPC request" << std::endl;
            continue;
        }

        hakoniwa::pdu::rpc::RpcResponse res;
        hakoniwa::pdu::rpc::ClientEventType event = hakoniwa::pdu::rpc::ClientEventType::NONE;
        std::string service_name;
        while (event == hakoniwa::pdu::rpc::ClientEventType::NONE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            event = client.poll(service_name, res);
        }

        if (event == hakoniwa::pdu::rpc::ClientEventType::RESPONSE_IN) {
            HakoCpp_AddTwoIntsResponse body;
            if (helper.get_response_body(res, body)) {
                std::cout << "sum=" << body.sum << std::endl;
            } else {
                std::cerr << "Failed to decode response" << std::endl;
            }
        } else {
            std::cerr << "RPC call failed or timed out (event: " << static_cast<int>(event) << ")" << std::endl;
        }
    }

    client.stop_all_services();
    endpoints->stop_all();
    return 0;
}
