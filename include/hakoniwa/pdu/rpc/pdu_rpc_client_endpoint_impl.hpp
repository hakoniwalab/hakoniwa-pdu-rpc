#pragma once

#include "hakoniwa/pdu/rpc/pdu_rpc_client_endpoint.hpp"
#include "hakoniwa/time_source/time_source.hpp"
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
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source);
    virtual ~PduRpcClientEndpointImpl();

    bool initialize(const nlohmann::json& service_config, int pdu_meta_data_size) override;
    bool call(const PduData& pdu, uint64_t timeout_usec) override;
    ClientEventType poll(RpcResponse& response) override;

    void create_request_buffer(Hako_uint8 opcode, bool is_cancel_request, PduData& pdu) override {
        PduKey pdu_key = {service_name_, client_name_ + "Req"};
        auto request_pdu_size = endpoint_->get_pdu_size(pdu_key);
        pdu.resize(request_pdu_size);
        HakoCpp_ServiceRequestHeader request_header;
        auto request_id = is_cancel_request ? client_state_.request_id : ++current_request_id_;
        request_header.request_id = request_id;
        request_header.client_name = client_name_;
        request_header.service_name = service_name_;
        request_header.opcode = opcode;
        request_header.status_poll_interval_msec = 0;
        (void)convertor_request_.cpp2pdu(request_header, reinterpret_cast<char*>(pdu.data()), request_pdu_size);
    }
    bool send_cancel_request()  override {
        PduData pdu;
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        if (client_state_.state != CLIENT_STATE_RUNNING) {
            std::cerr << "ERROR: Cannot send cancel request, client is not in RUNNING state." << std::endl;
            return false;
        }
        create_request_buffer(HAKO_SERVICE_OPERATION_CODE_CANCEL, true, pdu);
        try {
            if (send_request(pdu)) {
                client_state_.state = CLIENT_STATE_CANCELLING;
                return true;
            } else {
                std::cerr << "ERROR: send_request failed for cancel request." << std::endl;
                return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "ERROR: Failed to send cancel request: " << e.what() << std::endl;
            return false;
        }
    }


protected:
    void put_pending_response(const hakoniwa::pdu::PduKey& pdu_key, const PduData& pdu_data) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        pending_responses_.emplace_back(PendingResponse{pdu_key, pdu_data});
    }
    bool send_request(const PduData& pdu) override;
private:
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
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

    uint64_t current_timeout_usec_;
    uint64_t request_start_time_usec_;

    bool validate_header(HakoCpp_ServiceResponseHeader& header);
    ClientEventType handle_response_in(RpcResponse& request);
    ClientEventType handle_cancel_response(RpcResponse& request);
};

} // namespace hakoniwa::pdu::rpc