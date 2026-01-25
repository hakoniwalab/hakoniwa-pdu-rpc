#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <iostream>

int main() {
    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        "server_node", "config/sample/endpoints.json");
    if (endpoints->initialize() != HAKO_PDU_ERR_OK) {
        std::cerr << "Failed to initialize endpoints" << std::endl;
        return 1;
    }
    if (endpoints->start_all() != HAKO_PDU_ERR_OK) {
        std::cerr << "Failed to start endpoints" << std::endl;
        return 1;
    }

    hakoniwa::pdu::rpc::RpcServicesServer server(
        "server_node", "RpcServerEndpointImpl", "config/sample/simple-service.json", 1000);
    if (!server.initialize_services(endpoints)) {
        std::cerr << "Failed to initialize RPC services" << std::endl;
        return 1;
    }
    server.start_all_services();

    HakoRpcServiceServerTemplateType(AddTwoInts) helper;

    while (true) {
        hakoniwa::pdu::rpc::RpcRequest req;
        auto event = server.poll(req);
        if (event == hakoniwa::pdu::rpc::ServerEventType::REQUEST_IN) {
            HakoCpp_AddTwoIntsRequest body;
            if (!helper.get_request_body(req, body)) {
                std::cerr << "Failed to decode request" << std::endl;
                continue;
            }

            HakoCpp_AddTwoIntsResponse res;
            res.sum = body.a + body.b;

            if (!helper.reply(server, req,
                    hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
                    hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
                    res)) {
                std::cerr << "Failed to send reply" << std::endl;
            }
        }
    }
    return 0;
}
