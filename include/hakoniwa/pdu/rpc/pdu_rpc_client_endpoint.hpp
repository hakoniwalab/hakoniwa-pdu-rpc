#pragma once

#include "pdu_rpc_types.hpp"
#include <string>
#include <future>
#include <chrono>
#include <nlohmann/json_fwd.hpp>

namespace hakoniwa::pdu::rpc {
struct RpcResponse {
    HakoCpp_ServiceResponseHeader header;
    PduData pdu;
};

class IPduRpcClientEndpoint {
public:
    std::string get_service_name() const {
        return service_name_;
    }
    std::string get_client_name() const {
        return client_name_;
    }
    virtual bool initialize(const nlohmann::json& service_config) = 0;
    virtual std::future<PduData> call(const PduData& pdu, uint64_t timeout_usec) = 0;
    virtual void create_request_buffer(Hako_uint8 opcode, PduData& pdu) = 0;
    virtual void send_cancel_request(PduData& pdu) = 0;
protected:
    IPduRpcClientEndpoint(const std::string& service_name, const std::string& client_name, uint64_t delta_time_usec)
    : service_name_(service_name), client_name_(client_name), delta_time_usec_(delta_time_usec), current_request_id_(0) {}
    virtual ~IPduRpcClientEndpoint() = default;
    virtual void send_request(const PduData& pdu) = 0;
    
    std::string service_name_;
    std::string client_name_;
    uint64_t delta_time_usec_;
    Hako_uint32 current_request_id_ = 0;
};

} // namespace hakoniwa::pdu::rpc
