#pragma once

#include "pdu_rpc_types.hpp"
#include <string>
#include <optional>

namespace hakoniwa::pdu::rpc {

// Forward declaration
struct RpcRequest {
    HakoCpp_ServiceRequestHeader header;
    PduData pdu;
};

class IPduRpcServerEndpoint {
public:
protected:
    IPduRpcServerEndpoint(const std::string& service_name, size_t max_clients, const std::string& service_path, uint64_t delta_time_usec)
    : service_name_(service_name), max_clients_(max_clients), service_path_(service_path), delta_time_usec_(delta_time_usec) {}

protected:
    std::string service_name_;
    size_t max_clients_;
    std::string service_path_;
    uint64_t delta_time_usec_;
    virtual ~IPduRpcServerEndpoint() = default;

    virtual bool initialize_services() = 0;
    virtual bool start_rpc_service() = 0;
    virtual ServerEventType poll(RpcRequest& request) = 0;
    virtual void sleep(uint64_t time_usec) = 0;


    /**
     * @brief Sends a reply back to a specific client.
     * @param client_id The ID of the client to reply to.
     * @param pdu The PDU data to send as a reply.
     */
    virtual void send_reply(std::string client_name, const PduData& pdu) = 0;

    /**
     * @brief Notifies the client that its request was cancelled.
     * @param client_id The ID of the client.
     * @param pdu The PDU data to send.
     */
    virtual void send_cancel_reply(std::string client_name, const PduData& pdu) = 0;

public:
    std::string get_service_name() const {
        return service_name_;
    }
};

} // namespace hakoniwa::pdu::rpc
