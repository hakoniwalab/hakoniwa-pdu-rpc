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


PduRpcServerEndpointImpl::PduRpcServerEndpointImpl(std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<ITimeSource> time_source)
    : endpoint_(endpoint), time_source_(time_source) {
    if (endpoint_) {
        endpoint_->set_on_recv_callback([this](const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data) {
            this->pdu_recv_callback(pdu_key, data);
        });
    }
}

bool PduRpcServerEndpointImpl::initialize_services(const std::string& /*service_path*/, uint64_t /*delta_time_usec*/) {
    // Implementation depends on how services are defined and discovered.
    // For now, we assume services are started explicitly via start_rpc_service.
    return true;
}

void PduRpcServerEndpointImpl::sleep(uint64_t /*time_usec*/) {
    // This might involve a condition variable or a simple sleep,
    // depending on the threading model.
}

bool PduRpcServerEndpointImpl::start_rpc_service(const std::string& service_name, size_t max_clients) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (services_.find(service_name) != services_.end()) {
        return false; // Service already exists
    }
    services_[service_name] = {service_name, max_clients, {}};
    std::cout << "INFO: Started RPC service: " << service_name << std::endl;
    return true;
}

void PduRpcServerEndpointImpl::pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& pdu_key, std::span<const std::byte> data) {
    std::lock_guard<std::mutex> lock(mtx_);

    // This is a simplified interpretation of the incoming PDU.
    // A real implementation would need a robust way to map channel_id to service_name.
    // Here, we iterate through services to find a match based on channel_id, which is inefficient.
    std::string service_name_str;
    for (const auto& pair : services_) {
        // This check is a placeholder. A real mapping is needed.
        if (endpoint_->get_pdu_channel_id({"", pair.first}) == pdu_key.channel_id) {
            service_name_str = pair.first;
            break;
        }
    }
    if (service_name_str.empty()) {
         // Fallback for channel id not found in service mapping. This could be for client-side channels.
         return;
    }

    if (data.size() < sizeof(RpcRequestHeader)) {
        std::cerr << "ERROR: Received data too small for RPC header" << std::endl;
        return;
    }

    RpcRequestHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    
    // Naive client registration
    ClientId client_id = header.client_id;
    if (client_id == -1) { // Assuming -1 is for new clients
        client_id = next_client_id_++;
    }

    PduData pdu_data;
    if (header.data_len > 0) {
        pdu_data.resize(header.data_len);
        std::memcpy(pdu_data.data(), data.data() + sizeof(RpcRequestHeader), header.data_len);
    }
    
    pending_requests_.push_back({
        service_name_str,
        { client_id, std::move(pdu_data) }
    });
}


ServerEventType PduRpcServerEndpointImpl::poll(std::string& service_name) {
    std::lock_guard<std::mutex> lock(mtx_);

    // Check for timed-out requests and clean them up
    for (auto it = active_rpcs_.begin(); it != active_rpcs_.end(); ) {
        if (it->second->is_timed_out()) {
            it->second->set_status(RpcStatus::ERROR);
            // Potentially move to a "timed_out_rpcs" map for later handling
            it = active_rpcs_.erase(it);
        } else {
            ++it;
        }
    }

    if (pending_requests_.empty()) {
        return ServerEventType::NONE;
    }

    last_polled_request_ = std::move(pending_requests_.front());
    pending_requests_.erase(pending_requests_.begin());

    service_name = last_polled_request_->service_name;
    
    // Here, we need to differentiate between a new request and a cancellation request.
    // This requires more detail in the PDU header, which we assume is part of method_id.
    // For now, all incoming are considered REQUEST_IN.
    return ServerEventType::REQUEST_IN;
}

std::optional<RpcRequest> PduRpcServerEndpointImpl::recv_request() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (!last_polled_request_) {
        return std::nullopt;
    }

    auto req = std::move(last_polled_request_->request);
    last_polled_request_.reset();

    // Create and store a core RPC object to track this request
    auto rpc_core = std::make_shared<PduRpcCore>(req.pdu.size(), time_source_); // req.pdu.size() as a placeholder for request_id
    active_rpcs_[req.client_id] = rpc_core;

    return req;
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
