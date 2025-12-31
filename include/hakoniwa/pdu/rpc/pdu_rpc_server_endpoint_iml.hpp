#pragma once

#include "hakoniwa/pdu/rpc/pdu_rpc_server_endpoint.hpp"
#include "hakoniwa/pdu/rpc/pdu_rpc_core.hpp"
#include "hakoniwa/pdu/rpc/pdu_rpc_time.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>

namespace hakoniwa::pdu::rpc {

class PduRpcServerEndpointImpl : public IPduRpcServerEndpoint {
public:
    PduRpcServerEndpointImpl(
        const std::string& service_name, size_t max_clients, const std::string& service_path, uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<ITimeSource> time_source);
    virtual ~PduRpcServerEndpointImpl() = default;

    bool initialize_services() override;
    void sleep(uint64_t time_usec) override;

    bool start_rpc_service() override;

    ServerEventType poll(RpcRequest& request) override;

    void send_reply(ClientId client_id, const PduData& pdu) override;

    void send_cancel_reply(ClientId client_id, const PduData& pdu) override;

private:
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<ITimeSource> time_source_;
    std::map<std::string, RpcService> services_;
    std::map<ClientId, std::shared_ptr<PduRpcCore>> active_rpcs_;
    std::mutex mtx_;
    ClientId next_client_id_ = 0;

    // A queue to hold incoming requests detected by the callback
    struct PendingRequest {
        std::string service_name;
        RpcRequest request;
    };
    std::vector<PendingRequest> pending_requests_;
    std::optional<PendingRequest> last_polled_request_;
    
    static void pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data);
};

} // namespace hakoniwa::pdu::rpc