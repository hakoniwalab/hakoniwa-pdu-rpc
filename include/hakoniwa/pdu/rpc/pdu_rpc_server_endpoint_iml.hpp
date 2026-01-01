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

enum HakoServiceOperationCodeType {
    HAKO_SERVICE_OPERATION_CODE_REQUEST = 0,  // Standard service request
    HAKO_SERVICE_OPERATION_CODE_CANCEL,       // Cancel the currently active request
    HAKO_SERVICE_OPERATION_NUM
};

enum ServerState {
    SERVER_STATE_IDLE = 0,
    SERVER_STATE_RUNNING,
    SERVER_STATE_CANCELLING,
    SERVER_STATE_NUM
};

class PduRpcServerEndpointImpl : public IPduRpcServerEndpoint, public std::enable_shared_from_this<PduRpcServerEndpointImpl> {
public:
    PduRpcServerEndpointImpl(
        const std::string& server_name, const std::string& service_name, size_t max_clients, const std::string& service_path, uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<ITimeSource> time_source);
    virtual ~PduRpcServerEndpointImpl() = default;

    bool initialize_services() override;
    void sleep(uint64_t time_usec) override;

    bool start_rpc_service() override;

    ServerEventType poll(RpcRequest& request) override;

    void send_reply(ClientId client_id, const PduData& pdu) override;

    void send_cancel_reply(ClientId client_id, const PduData& pdu) override;

protected:
    void put_pending_request(const hakoniwa::pdu::PduKey& pdu_key, const PduData& pdu_data) {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_requests_.emplace_back(PendingRequest{pdu_key, pdu_data});
    }
private:
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<ITimeSource> time_source_;
    std::map<ClientId, std::shared_ptr<PduRpcCore>> active_rpcs_;
    std::mutex mtx_;

    // A queue to hold incoming requests detected by the callback
    struct PendingRequest {
        hakoniwa::pdu::PduKey pdu_key;
        PduData pdu_data;
    };
    std::map<std::string, ServerState> server_states_;
    std::vector<std::string> registered_clients_;
    std::vector<PendingRequest> pending_requests_;
    hako::pdu::PduConvertor<HakoCpp_ServiceRequestHeader, hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader> convertor_request_;
    hako::pdu::PduConvertor<HakoCpp_ServiceResponseHeader, hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> convertor_response_;
    
    static void pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data);
    static std::vector<std::shared_ptr<PduRpcServerEndpointImpl>> instances_;


    bool validate_header(HakoCpp_ServiceRequestHeader& header);
};

} // namespace hakoniwa::pdu::rpc