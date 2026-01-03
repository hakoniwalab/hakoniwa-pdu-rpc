#pragma once

#include "hakoniwa/pdu/rpc/pdu_rpc_client_endpoint.hpp"
#include "hakoniwa/pdu/rpc/pdu_rpc_time.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <nlohmann/json_fwd.hpp>

namespace hakoniwa::pdu::rpc {


enum ClientState {
    CLIENT_STATE_IDLE = 0,
    CLIENT_STATE_RUNNING,
    CLIENT_STATE_CANCELLING,
    CLIENT_STATE_NUM
};
struct ClientProcessingStatus {
    Hako_uint32 request_id;
    ClientState state;
};

class PduRpcClientEndpointImpl : public IPduRpcClientEndpoint, public std::enable_shared_from_this<PduRpcClientEndpointImpl> {
public:
    PduRpcClientEndpointImpl(
        const std::string& service_name, const std::string& client_name, uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<ITimeSource> time_source);
    virtual ~PduRpcClientEndpointImpl() = default;

    bool initialize(const nlohmann::json& service_config) override;

    std::future<PduData> call(const PduData& pdu, uint64_t timeout_usec) override;

    void create_request_buffer(Hako_uint8 opcode, PduData& pdu) override {
        PduKey pdu_key = {service_name_, client_name_ + "Req"};
        auto request_pdu_size = endpoint_->get_pdu_size(pdu_key);
        pdu.resize(request_pdu_size);
        HakoCpp_ServiceRequestHeader request_header;
        request_header.request_id = ++current_request_id_;
        request_header.client_name = client_name_;
        request_header.service_name = service_name_;
        request_header.opcode = opcode;
        request_header.status_poll_interval_msec = 0;
        convertor_request_.cpp2pdu(request_header, reinterpret_cast<char*>(pdu.data()), request_pdu_size);
    }
    void send_cancel_request(PduData& pdu)  override {
        create_request_buffer(HAKO_SERVICE_OPERATION_CODE_CANCEL, pdu);
        send_request(pdu);
    }


protected:
    void put_pending_response(const hakoniwa::pdu::PduKey& pdu_key, const PduData& pdu_data) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        pending_responses_.emplace_back(PendingResponse{pdu_key, pdu_data});
    }
    void send_request(const PduData& pdu) override;
private:
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<ITimeSource> time_source_;
    std::recursive_mutex mtx_;

    // A queue to hold incoming requests detected by the callback
    struct PendingResponse {
        hakoniwa::pdu::PduKey pdu_key;
        PduData pdu_data;
    };
    ClientProcessingStatus client_state_;
    std::vector<PendingResponse> pending_responses_;
    hako::pdu::PduConvertor<HakoCpp_ServiceRequestHeader, hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader> convertor_request_;
    hako::pdu::PduConvertor<HakoCpp_ServiceResponseHeader, hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> convertor_response_;
    
    static void pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data);
    static std::vector<std::shared_ptr<PduRpcClientEndpointImpl>> instances_;


    bool validate_header(HakoCpp_ServiceResponseHeader& header);
    ClientEventType handle_response_in(RpcResponse& request);
    ClientEventType handle_cancel_response(RpcResponse& request);
};

} // namespace hakoniwa::pdu::rpc