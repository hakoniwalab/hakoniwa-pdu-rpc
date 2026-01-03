#pragma once

#include "pdu_rpc_types.hpp"
#include <string>
#include <optional>
#include <nlohmann/json_fwd.hpp>

namespace hakoniwa::pdu::rpc {

// Forward declaration
struct RpcRequest {
    HakoCpp_ServiceRequestHeader header;
    PduData pdu;
};

class IPduRpcServerEndpoint {
public:
    std::string get_service_name() const {
        return service_name_;
    }
    virtual bool initialize(const nlohmann::json& service_config) = 0;
    virtual ServerEventType poll(RpcRequest& request) = 0;
    virtual void create_reply_buffer(const HakoCpp_ServiceRequestHeader& header, Hako_uint8 status, Hako_int32 result_code, PduData& pdu) = 0;


    virtual void send_reply(std::string client_name, const PduData& pdu) = 0;
    virtual void send_cancel_reply(std::string client_name, const PduData& pdu) = 0;
protected:
    IPduRpcServerEndpoint(const std::string& service_name, uint64_t delta_time_usec)
    : service_name_(service_name), max_clients_(1), delta_time_usec_(delta_time_usec) {}
    virtual ~IPduRpcServerEndpoint() = default;

    virtual void send_error_reply(const HakoCpp_ServiceRequestHeader& header, Hako_int32 result_code) = 0;

    std::string service_name_;
    size_t max_clients_;
    uint64_t delta_time_usec_;


};

} // namespace hakoniwa::pdu::rpc
