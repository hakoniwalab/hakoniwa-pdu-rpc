#pragma once

#include "rpc_types.hpp"
#include <string>
#include <nlohmann/json_fwd.hpp>


namespace hakoniwa::pdu::rpc {

class IRpcClientEndpoint {
public:
    virtual ~IRpcClientEndpoint() = default;

    virtual bool initialize(const nlohmann::json& service_config, int pdu_meta_data_size) = 0;
    virtual bool call(const PduData& pdu, uint64_t timeout_usec) = 0;
    virtual ClientEventType poll(RpcResponse& response) = 0;
    virtual bool send_cancel_request() = 0;
    virtual void create_request_buffer(Hako_uint8 opcode, bool is_cancel_request, PduData& pdu) = 0;
    virtual void clear_pending_responses() = 0;

    const std::string& get_service_name() const { return service_name_; }
    const std::string& get_client_name() const { return client_name_; }
protected:
    IRpcClientEndpoint(const std::string& service_name, const std::string& client_name, uint64_t delta_time_usec)
        : service_name_(service_name), client_name_(client_name), delta_time_usec_(delta_time_usec) {}

    std::string service_name_;
    std::string client_name_;
    uint64_t delta_time_usec_;
};

}
