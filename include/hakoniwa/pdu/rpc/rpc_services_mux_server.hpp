#pragma once

#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hakoniwa::pdu::rpc {

struct RpcMuxRequest {
    std::uint64_t connection_id{0};
    RpcRequest request;
};

// Server-side transport owner for EndpointCommMultiplexer sessions.
//
// One listening endpoint accepts multiple client connections. Each accepted
// connection becomes an already-opened Endpoint and gets its own
// RpcServicesServer adapter. Replies are routed by the connection_id returned
// together with the request.
class RpcServicesMuxServer {
public:
    RpcServicesMuxServer(
        std::string node_id,
        std::string impl_type,
        std::string service_config_path,
        std::string endpoint_mux_config_path,
        std::uint64_t delta_time_usec,
        std::string time_source_type = "real");
    ~RpcServicesMuxServer();

    RpcServicesMuxServer(const RpcServicesMuxServer&) = delete;
    RpcServicesMuxServer& operator=(const RpcServicesMuxServer&) = delete;

    bool initialize();
    bool start();
    void stop();

    ServerEventType poll(RpcMuxRequest& request);

    bool create_reply_buffer(
        const RpcMuxRequest& request,
        Hako_uint8 status,
        Hako_int32 result_code,
        PduData& pdu);
    bool send_reply(const RpcMuxRequest& request, const PduData& pdu);
    bool send_cancel_reply(const RpcMuxRequest& request, const PduData& pdu);

    std::size_t connected_count() const;
    std::size_t expected_count() const;
    bool is_ready() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hakoniwa::pdu::rpc
