#include "hakoniwa/pdu/rpc/pdu_rpc_server_endpoint_iml.hpp"
#include <iostream>
#include <cstring>

namespace hakoniwa::pdu::rpc {

// Assuming a simple PDU header structure for requests
// In a real scenario, this would be more complex, probably defined in a shared header.
struct RpcRequestHeader {
    uint32_t method_id;
    int64_t request_id; // Corresponds to RequestId
    int32_t client_id;  // Corresponds to ClientId
    uint32_t data_len;
};
// And for replies
struct RpcReplyHeader {
    int64_t request_id;
    int32_t result_code;
    uint32_t data_len;
};


PduRpcServerEndpointImpl::PduRpcServerEndpointImpl(
    const std::string& service_name, size_t max_clients, const std::string& service_path, uint64_t delta_time_usec,
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<ITimeSource> time_source)
    : IPduRpcServerEndpoint(service_name, max_clients, service_path, delta_time_usec),
      endpoint_(endpoint), time_source_(time_source) {
    if (endpoint_) {
        endpoint_->set_on_recv_callback([this](const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data) {
            PduRpcServerEndpointImpl::pdu_recv_callback(pdu_key, data);
        });
    }
}

bool PduRpcServerEndpointImpl::initialize_services() {
    if (!endpoint_) {
        std::cerr << "ERROR: Endpoint is not initialized." << std::endl;
        return false;
    }
    // Open the communication endpoint using the service_path_ provided at construction
    HakoPduErrorType err = endpoint_->open(service_path_);
    if (err != HAKO_PDU_ERR_OK) {
        std::cerr << "ERROR: Failed to open endpoint for service " << service_name_ << ": " << static_cast<int>(err) << std::endl;
        return false;
    }
    // Service definition might be added here if needed in the future,
    // but for now, the endpoint is initialized.
    return true;
}

void PduRpcServerEndpointImpl::sleep(uint64_t /*time_usec*/) {
    // This might involve a condition variable or a simple sleep,
    // depending on the threading model.
}

bool PduRpcServerEndpointImpl::start_rpc_service() {
    if (!endpoint_) {
        std::cerr << "ERROR: Endpoint is not initialized." << std::endl;
        return false;
    }
    // Start the communication endpoint
    HakoPduErrorType err = endpoint_->start();
    if (err != HAKO_PDU_ERR_OK) {
        std::cerr << "ERROR: Failed to start endpoint for service " << service_name_ << ": " << static_cast<int>(err) << std::endl;
        return false;
    }
    return true;
}

void PduRpcServerEndpointImpl::pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data) {

}


ServerEventType PduRpcServerEndpointImpl::poll(RpcRequest& request) 
{

    
    return ServerEventType::REQUEST_IN;
}

void PduRpcServerEndpointImpl::send_reply(ClientId client_id, const PduData& pdu) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = active_rpcs_.find(client_id);
    if (it == active_rpcs_.end()) {
        std::cerr << "ERROR: Can't send reply to unknown or completed client_id: " << client_id << std::endl;
        return;
    }
    
    // Construct the reply PDU
    RpcReplyHeader reply_header;
    reply_header.request_id = it->second->get_request_id();
    reply_header.result_code = static_cast<int32_t>(RpcResultCode::OK);
    reply_header.data_len = pdu.size();

    PduData reply_pdu(sizeof(reply_header) + pdu.size());
    std::memcpy(reply_pdu.data(), &reply_header, sizeof(reply_header));
    if (!pdu.empty()) {
        std::memcpy(reply_pdu.data() + sizeof(reply_header), pdu.data(), pdu.size());
    }

    // This is a placeholder. Need to resolve client_id to a specific channel.
    //hakoniwa::pdu::PduKey pdu_key = {"", "reply_channel_for_client_" + std::to_string(client_id)};
    //endpoint_->send(pdu_key, std::as_bytes(std::span(reply_pdu)));

    active_rpcs_.erase(it);
    std::cout << "INFO: Sent reply to client_id: " << client_id << std::endl;
}

void PduRpcServerEndpointImpl::send_cancel_reply(ClientId client_id, const PduData& pdu) {
    // Similar to send_reply, but with a CANCELED result code
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = active_rpcs_.find(client_id);
    if (it == active_rpcs_.end()) {
        return; // Request might have already completed or timed out
    }
    
    RpcReplyHeader reply_header;
    reply_header.request_id = it->second->get_request_id();
    reply_header.result_code = static_cast<int32_t>(RpcResultCode::CANCELED);
    reply_header.data_len = pdu.size();
    
    PduData reply_pdu(sizeof(reply_header) + pdu.size());
    std::memcpy(reply_pdu.data(), &reply_header, sizeof(reply_header));
    if (!pdu.empty()) {
        std::memcpy(reply_pdu.data() + sizeof(reply_header), pdu.data(), pdu.size());
    }

    // Placeholder for sending logic
    //hakoniwa::pdu::PduKey pdu_key = {"", "reply_channel_for_client_" + std::to_string(client_id)};
    //endpoint_->send(pdu_key, std::as_bytes(std::span(reply_pdu)));

    active_rpcs_.erase(it);
}

} // namespace hakoniwa::pdu::rpc
