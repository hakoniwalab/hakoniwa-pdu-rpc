#include "hakoniwa/pdu/rpc/pdu_rpc_server_endpoint_iml.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <cstring>

namespace hakoniwa::pdu::rpc {

std::vector<std::shared_ptr<PduRpcServerEndpointImpl>> PduRpcServerEndpointImpl::instances_;

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
        endpoint_->set_on_recv_callback([this](const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) {
            PduRpcServerEndpointImpl::pdu_recv_callback(resolved_pdu_key, data);
        });
    }
    instances_.push_back(shared_from_this());
}

bool PduRpcServerEndpointImpl::initialize_services() {
    if (!endpoint_) {
        std::cerr << "ERROR: Endpoint is not initialized." << std::endl;
        return false;
    }
    std::ifstream ifs(this->service_path_);
    if (!ifs.is_open()) {
        std::cerr << "ERROR: Failed to open service definition file: " << this->service_path_ << std::endl;
        return false;
    }

    nlohmann::json services_json;
    try {
        ifs >> services_json;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse service definition file: " << e.what() << std::endl;
        return false;
    }

    for (const auto& service : services_json["services"]) {
        std::string service_name = service["name"];
        std::string service_type = service["type"];
        auto& pdu_def = endpoint_->get_pdu_definition();

        for (const auto& client : service["clients"]) {
            std::string client_name = client["name"];
            // Request PDU
            PduDef req_def;
            req_def.org_name = client_name + "Req";
            req_def.name = service_name + "_" + req_def.org_name;
            req_def.channel_id = client["requestChannelId"];
            req_def.pdu_size = service["pduSize"]["client"]["baseSize"].get<size_t>() + service["pduSize"]["client"]["heapSize"].get<size_t>();
            req_def.method_type = "RPC";
            pdu_def.add_definition(service_name, req_def);

            // Response PDU
            PduDef res_def;
            res_def.org_name = client_name + "Res";
            res_def.name = service_name + "_" + res_def.org_name;
            res_def.channel_id = client["responseChannelId"];
            res_def.pdu_size = service["pduSize"]["server"]["baseSize"].get<size_t>() + service["pduSize"]["server"]["heapSize"].get<size_t>();
            res_def.method_type = "RPC";
            pdu_def.add_definition(service_name, res_def);
        }
    }

    return true;
}

void PduRpcServerEndpointImpl::sleep(uint64_t time_usec) {
    time_source_->sleep(time_usec);
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
    bool running = false;
    while (running == false) {
        (void)endpoint_->is_running(running);
        if (running) {
            break;
        }
        this->sleep(1000); // Sleep for 1ms
    }
    return true;
}

void PduRpcServerEndpointImpl::pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) 
{
    for (const auto& instance : instances_) {
        // robot_name = service_name
        if (instance->get_service_name() != resolved_pdu_key.robot) {
            continue;
        }
        assert(instance->endpoint_ != nullptr);
        // channel_Id = client_request_channel_id
        std::string pdu_name = instance->endpoint_->get_pdu_name(resolved_pdu_key);
        hakoniwa::pdu::PduKey pdu_key = {resolved_pdu_key.robot, pdu_name};
        PduData pdu_data = {};
        pdu_data.resize(data.size());
        std::memcpy(pdu_data.data(), data.data(), data.size());
        instance->put_pending_request(pdu_key, pdu_data);
    }
}


ServerEventType PduRpcServerEndpointImpl::poll(RpcRequest& request) 
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (pending_requests_.empty()) {
        return ServerEventType::NONE;
    }
    PendingRequest pending_request = std::move(pending_requests_.front());
    pending_requests_.erase(pending_requests_.begin());
    request.pdu = std::move(pending_request.pdu_data);

    convertor_request_.pdu2cpp(reinterpret_cast<char*>(request.pdu.data()), request.header);
    if (!validate_header(request.header)) {
        std::cerr << "ERROR: Invalid request header received and ignored" << std::endl;
        //ignore invalid request
        //TODO send error reply
        return ServerEventType::NONE;
    }
    if (request.header.opcode == HAKO_SERVICE_OPERATION_CODE_CANCEL) {
        if (server_states_[request.header.client_name] == ServerState::SERVER_STATE_RUNNING) {
            server_states_[request.header.client_name] = ServerState::SERVER_STATE_CANCELLING;
            std::cout << "INFO: Received cancel request for client: " << request.header.client_name << std::endl;
            return ServerEventType::REQUEST_CANCEL;
        }
        else if (server_states_[request.header.client_name] == ServerState::SERVER_STATE_IDLE) {
            // Already idle, nothing to cancel
            // client must get normal reply and cancel request must be ignored
            std::cerr << "WARNING: Received cancel request while idle for client: " << request.header.client_name << std::endl;
            return ServerEventType::NONE;
        }
        else {
            // Already cancelling
            //TODO reply busy reply
            std::cerr << "WARNING: Received cancel request while already cancelling for client: " << request.header.client_name << std::endl;
            return ServerEventType::NONE;
        }
    }
    else { // REQUEST
        if (server_states_[request.header.client_name] == ServerState::SERVER_STATE_IDLE) {
            std::cout << "INFO: Received request for client: " << request.header.client_name << std::endl;
            server_states_[request.header.client_name] = ServerState::SERVER_STATE_RUNNING;
            return ServerEventType::REQUEST_IN;
        }
        else if (server_states_[request.header.client_name] == ServerState::SERVER_STATE_RUNNING) {
            std::cerr << "WARNING: Received request while previous request is still running for client: " << request.header.client_name << std::endl;
            //TODO reply busy reply
            return ServerEventType::NONE;
        }
        else { // CANCELING
            std::cerr << "WARNING: Received request while previous request is cancelling for client: " << request.header.client_name << std::endl;
            //TODO reply busy reply
            return ServerEventType::NONE;
        }
    }
}

void PduRpcServerEndpointImpl::send_reply(std::string client_name, const PduData& pdu) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (server_states_.find(client_name) == server_states_.end()) {
        std::cerr << "ERROR: Unknown client_name: " << client_name << std::endl;
        return;
    }
    else if (server_states_[client_name] != ServerState::SERVER_STATE_RUNNING) {
        std::cerr << "ERROR: Cannot send reply, server state is not RUNNING for client: " << client_name << std::endl;
        return;
    }
    
    server_states_[client_name] = ServerState::SERVER_STATE_IDLE;
    std::cout << "INFO: Reset state to IDLE for client: " << client_name << std::endl;

    // Construct the reply PDU
    RpcReplyHeader reply_header;
    //TODO
    
    PduData reply_pdu(sizeof(reply_header) + pdu.size());
    std::memcpy(reply_pdu.data(), &reply_header, sizeof(reply_header));
    if (!pdu.empty()) {
        std::memcpy(reply_pdu.data() + sizeof(reply_header), pdu.data(), pdu.size());
    }

    std::cout << "INFO: Sent reply to client_name: " << client_name << std::endl;
}

void PduRpcServerEndpointImpl::send_cancel_reply(std::string client_name, const PduData& pdu) {
    // Similar to send_reply, but with a CANCELED result code
    std::lock_guard<std::mutex> lock(mtx_);
    if (server_states_.find(client_name) == server_states_.end()) {
        std::cerr << "ERROR: Unknown client_name: " << client_name << std::endl;
        return;
    }
    else if (server_states_[client_name] != ServerState::SERVER_STATE_CANCELLING) {
        std::cerr << "ERROR: Cannot send reply, server state is not CANCELLING for client: " << client_name << std::endl;
        return;
    }
    
    // Reset server state to IDLE
    server_states_[client_name] = ServerState::SERVER_STATE_IDLE;
    std::cout << "INFO: Reset state to IDLE for client: " << client_name << " after cancellation" << std::endl;

    RpcReplyHeader reply_header;
    //TODO
    
    PduData reply_pdu(sizeof(reply_header) + pdu.size());
    std::memcpy(reply_pdu.data(), &reply_header, sizeof(reply_header));
    if (!pdu.empty()) {
        std::memcpy(reply_pdu.data() + sizeof(reply_header), pdu.data(), pdu.size());
    }

    std::cout << "INFO: Sent cancel reply to client_name: " << client_name << std::endl;
}

bool PduRpcServerEndpointImpl::validate_header(HakoCpp_ServiceRequestHeader& header)
{
    if (header.service_name != this->service_name_) {
        std::cerr << "ERROR: service_name is invalid: " << header.service_name << std::endl;
        return false;
    }
    if (std::find(registered_clients_.begin(), registered_clients_.end(), header.client_name) == registered_clients_.end()) {
        std::cerr << "ERROR: client_name is invalid: " << header.client_name << std::endl;
        return false;
    }
    if (header.opcode >= HakoServiceOperationCodeType::HAKO_SERVICE_OPERATION_NUM) {
        std::cerr << "ERROR: opcode is invalid: " << header.opcode << std::endl;
        return false;
    }
    return true;
} 

} // namespace hakoniwa::pdu::rpc