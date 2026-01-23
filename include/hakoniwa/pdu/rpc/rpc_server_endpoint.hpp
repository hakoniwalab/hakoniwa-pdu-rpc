#pragma once
#include "rpc_types.hpp"
#include <string>
#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace hakoniwa::pdu::rpc {

class IRpcServerEndpoint {
public:
    virtual ~IRpcServerEndpoint() = default;

    virtual bool initialize(const nlohmann::json& service_config, int pdu_meta_data_size, std::optional<std::string> client_node_id = std::nullopt) = 0;

    virtual ServerEventType poll(RpcRequest& request) = 0;

    virtual void send_reply(std::string client_name, const PduData& pdu) = 0;
    virtual void send_cancel_reply(std::string client_name, const PduData& pdu) = 0;
    virtual void create_reply_buffer(const HakoCpp_ServiceRequestHeader& header, Hako_uint8 status, Hako_int32 result_code, PduData& pdu) = 0;
    virtual void clear_pending_requests() = 0;
    const std::string& get_service_name() const { return service_name_; }
protected:
    IRpcServerEndpoint(const std::string& service_name, uint64_t delta_time_usec)
        : service_name_(service_name), delta_time_usec_(delta_time_usec) {}
    std::string service_name_;
    uint64_t delta_time_usec_;
};

}
