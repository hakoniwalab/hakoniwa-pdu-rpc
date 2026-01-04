#pragma once

#include "hakoniwa/pdu/rpc/pdu_rpc_server_endpoint.hpp"
#include "hakoniwa/time_source/time_source.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <nlohmann/json_fwd.hpp>

namespace hakoniwa::pdu::rpc {


enum ServerState {
    SERVER_STATE_IDLE = 0,
    SERVER_STATE_RUNNING,
    SERVER_STATE_CANCELLING,
    SERVER_STATE_NUM
};
struct ServerProcessingStatus {
    Hako_uint32 request_id;
    ServerState state;
};

class PduRpcServerEndpointImpl : public IPduRpcServerEndpoint, public std::enable_shared_from_this<PduRpcServerEndpointImpl> {
public:
    PduRpcServerEndpointImpl(
        const std::string& service_name, uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source);
    virtual ~PduRpcServerEndpointImpl();

    bool initialize(const nlohmann::json& service_config, int pdu_meta_data_size) override;

    ServerEventType poll(RpcRequest& request) override;
    void create_reply_buffer(const HakoCpp_ServiceRequestHeader& header, Hako_uint8 status, Hako_int32 result_code, PduData& pdu) override {
        PduKey pdu_key = {header.service_name, header.client_name + "Res"};
        auto response_pdu_size = endpoint_->get_pdu_size(pdu_key);
        pdu.resize(response_pdu_size);
        HakoCpp_ServiceResponseHeader response_header;
        response_header.request_id = header.request_id;
        response_header.client_name = header.client_name;
        response_header.service_name = header.service_name;
        response_header.status = status;
        response_header.processing_percentage = 100;
        response_header.result_code = result_code;
        convertor_response_.cpp2pdu(response_header, reinterpret_cast<char*>(pdu.data()), response_pdu_size);
    }
    void send_error_reply(const HakoCpp_ServiceRequestHeader& header, Hako_int32 result_code) override {
        PduData pdu;
        create_reply_buffer(header, HAKO_SERVICE_STATUS_ERROR, result_code, pdu);
        send_reply(header.client_name, pdu);
    }

    void send_reply(std::string client_name, const PduData& pdu) override;

    void send_cancel_reply(std::string client_name, const PduData& pdu) override;

protected:
    void put_pending_request(const hakoniwa::pdu::PduKey& pdu_key, const PduData& pdu_data) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        pending_requests_.emplace_back(PendingRequest{pdu_key, pdu_data});
    }
private:
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::recursive_mutex mtx_;

    // A queue to hold incoming requests detected by the callback
    struct PendingRequest {
        hakoniwa::pdu::PduKey pdu_key;
        PduData pdu_data;
    };
    std::map<std::string, ServerProcessingStatus> server_states_;
    std::vector<std::string> registered_clients_;
    std::vector<PendingRequest> pending_requests_;
    hako::pdu::PduConvertor<HakoCpp_ServiceRequestHeader, hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader> convertor_request_;
    hako::pdu::PduConvertor<HakoCpp_ServiceResponseHeader, hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> convertor_response_;
    
    static void pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data);
    static std::vector<std::shared_ptr<PduRpcServerEndpointImpl>> instances_;


    bool validate_header(HakoCpp_ServiceRequestHeader& header);
    ServerEventType handle_request_in(RpcRequest& request);
    ServerEventType handle_cancel_request(RpcRequest& request);
};

} // namespace hakoniwa::pdu::rpc