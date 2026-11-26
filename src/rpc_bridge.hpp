#pragma once

#include "hakoniwa/pdu/rpc/rpc_client_endpoint.hpp"
#include "hakoniwa/pdu/rpc/rpc_server_endpoint.hpp"

#include <deque>
#include <optional>

namespace hakoniwa::pdu::rpc {

class RpcBridge {
public:
    RpcBridge(IRpcServerEndpoint& server, IRpcClientEndpoint& client);
    void poll();

private:
    struct PendingRequest {
        RpcRequest request;
        bool cancel_requested = false;
        bool cancel_sent = false;
    };

    void receive_request();
    void receive_response();
    void forward_request();
    void reply_with_error(const HakoCpp_ServiceRequestHeader& request, Hako_int32 result);

    IRpcServerEndpoint& server_;
    IRpcClientEndpoint& client_;
    std::deque<PendingRequest> pending_;
    // The configured downstream client can carry one request at a time.
    std::optional<Hako_uint32> outgoing_id_;
};

} // namespace hakoniwa::pdu::rpc
