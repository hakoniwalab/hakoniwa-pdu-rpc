#pragma once

#include "pdu_rpc_types.hpp"
#include <string>
#include <optional>

namespace hakoniwa::pdu::rpc {

// Forward declaration
struct RpcRequest {
    ClientId client_id;
    PduData pdu;
};

class IPduRpcServerEndpoint {
public:
    virtual ~IPduRpcServerEndpoint() = default;

    virtual bool initialize_services(const std::string& service_path, uint64_t delta_time_usec) = 0;
    virtual void sleep(uint64_t time_usec) = 0;

    virtual bool start_rpc_service(const std::string& service_name, size_t max_clients) = 0;

    /**
     * @brief Polls for incoming RPC events.
     * @return The type of event and the service name that occurred.
     */
    virtual ServerEventType poll(std::string& service_nam) = 0;

    /**
     * @brief Receives a request from a client.
     * Should be called after poll() returns REQUEST_IN.
     * @return An optional RpcRequest containing the client ID and PDU data.
     */
    virtual std::optional<RpcRequest> recv_request() = 0;

    /**
     * @brief Sends a reply back to a specific client.
     * @param client_id The ID of the client to reply to.
     * @param pdu The PDU data to send as a reply.
     */
    virtual void send_reply(ClientId client_id, const PduData& pdu) = 0;

    /**
     * @brief Notifies the client that its request was cancelled.
     * @param client_id The ID of the client.
     * @param pdu The PDU data to send.
     */
    virtual void send_cancel_reply(ClientId client_id, const PduData& pdu) = 0;
};

} // namespace hakoniwa::pdu::rpc
