#pragma once

#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"

/** Shorthand macro to resolve PDU type */
#define HAKO_RPC_SERVICE_SERVER_TYPE(type) hako::pdu::msgs::hako_srv_msgs::type

namespace hakoniwa::pdu::rpc {
template<typename CppReqPacketType, typename CppResPacketType, 
         typename CppReqBodyType, typename CppResBodyType, 
         typename ConvertorReq, typename ConvertorRes>
class HakoRpcAssetServiceServer {
public:
    HakoRpcAssetServiceServer() = default;
    virtual ~HakoRpcAssetServiceServer() = default;

    // get HakoCpp_AddTwoIntsRequest from packet
    bool get_request_body(RpcRequest& request, CppReqBodyType& req_body) {
        ConvertorReq convertor_request;
        CppReqPacketType request_packet;
        bool ret = convertor_request.pdu2cpp(reinterpret_cast<char*>(request.pdu.data()), request_packet);
        if (!ret) {
            std::cerr << "ERROR: Failed to convert request PDU to C++ type." << std::endl;
            return false;
        }
        req_body = request_packet.body;
        return true;
    }
    bool get_response_body(RpcResponse& response, CppResBodyType& res_body) {
        ConvertorRes convertor_response;
        CppResPacketType response_packet;
        bool ret = convertor_response.pdu2cpp(reinterpret_cast<char*>(response.pdu.data()), response_packet);
        if (!ret) {
            std::cerr << "ERROR: Failed to convert response PDU to C++ type." << std::endl;
            return false;
        }
        res_body = response_packet.body;
        return true;
    }
    bool set_response_body(RpcServicesServer& server, RpcRequest& request, Hako_uint8 status, Hako_int32 result_code, const CppResBodyType& res_body, PduData& response_pdu) {
        server.create_reply_buffer(request.header, status, result_code, response_pdu);
        ConvertorRes convertor_response;
        CppResPacketType response_packet;
        auto ret = convertor_response.pdu2cpp(reinterpret_cast<char*>(response_pdu.data()), response_packet);
        if (!ret) {
            std::cerr << "ERROR: Failed to convert response PDU to C++ type." << std::endl;
            return false;
        }
        response_packet.body = res_body;
        int size = convertor_response.cpp2pdu(response_packet, reinterpret_cast<char*>(response_pdu.data()), response_pdu.size());
        if (size < 0) {
            std::cerr << "ERROR: Failed to convert response C++ type to PDU." << std::endl;
            return false;
        }
        return true;
    }
    bool set_request_body(RpcServicesClient& client, const std::string& service_name, CppReqBodyType& req_body, PduData& request_pdu) {
        client.create_request_buffer(service_name, request_pdu);
        ConvertorReq convertor_request;
        CppReqPacketType request_packet;
        auto ret = convertor_request.pdu2cpp(reinterpret_cast<char*>(request_pdu.data()), request_packet);
        if (!ret) {
            std::cerr << "ERROR: Failed to convert request PDU to C++ type." << std::endl;
            return false;
        }
        request_packet.body = req_body;
        int size = convertor_request.cpp2pdu(request_packet, reinterpret_cast<char*>(request_pdu.data()), request_pdu.size());
        if (size < 0) {
            std::cerr << "ERROR: Failed to convert request C++ type to PDU." << std::endl;
            return false;
        }
        return true;
    }

    bool call(RpcServicesClient& client, const std::string& service_name, CppReqBodyType& req_body, uint64_t timeout_usec) {
        hakoniwa::pdu::rpc::PduData request_pdu;
        bool set_req_body = set_request_body(client, service_name, req_body, request_pdu);
        if (!set_req_body) {
            std::cerr << "ERROR: Failed to set request body." << std::endl;
            return false;
        }
        return client.call(service_name, request_pdu, timeout_usec);
    }
    bool reply(RpcServicesServer& server, RpcRequest& request, Hako_uint8 status, Hako_int32 result_code, const CppResBodyType& res_body) {
        hakoniwa::pdu::rpc::PduData response_pdu;
        bool set_res_body = set_response_body(server, request, status, result_code, res_body, response_pdu);
        if (!set_res_body) {
            std::cerr << "ERROR: Failed to set response body." << std::endl;
            return false;
        }
        server.send_reply(request.header, response_pdu);
        return true;
    }

};

} // namespace hakoniwa::pdu::rpc


/** Template instantiation helper */
#define HakoRpcServiceServerTemplateType(SRVNAME) \
    hakoniwa::pdu::rpc::HakoRpcAssetServiceServer< \
        HakoCpp_##SRVNAME##RequestPacket, \
        HakoCpp_##SRVNAME##ResponsePacket, \
        HakoCpp_##SRVNAME##Request, \
        HakoCpp_##SRVNAME##Response, \
        HAKO_RPC_SERVICE_SERVER_TYPE(SRVNAME##RequestPacket), \
        HAKO_RPC_SERVICE_SERVER_TYPE(SRVNAME##ResponsePacket)>

