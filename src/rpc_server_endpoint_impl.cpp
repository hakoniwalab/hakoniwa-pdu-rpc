#include "hakoniwa/pdu/rpc/rpc_server_endpoint_impl.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <algorithm>

namespace hakoniwa::pdu::rpc {

std::vector<std::shared_ptr<RpcServerEndpointImpl>> RpcServerEndpointImpl::instances_;

RpcServerEndpointImpl::RpcServerEndpointImpl(
    const std::string& service_name, uint64_t delta_time_usec,
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& endpoint, std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source)
 : IRpcServerEndpoint(service_name, delta_time_usec),
      endpoint_(endpoint), time_source_(time_source) {
    if (endpoint_) {
        endpoint_->set_on_recv_callback([this](const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) {
            RpcServerEndpointImpl::pdu_recv_callback(resolved_pdu_key, data);
        });
    }
}

RpcServerEndpointImpl::~RpcServerEndpointImpl() {
    auto it = std::remove_if(instances_.begin(), instances_.end(),
        [this](const std::shared_ptr<RpcServerEndpointImpl>& p) {
            return p.get() == this;
        });
    instances_.erase(it, instances_.end());
}


bool RpcServerEndpointImpl::initialize(const nlohmann::json& service_config, int pdu_meta_data_size) {
    if (!endpoint_) {
        std::cerr << "ERROR: Endpoint is not initialized." << std::endl;
        return false;
    }
    instances_.push_back(shared_from_this());

    try {
        max_clients_ = service_config["maxClients"].get<size_t>();
        std::string service_name = service_config["name"];
        std::string service_type = service_config["type"];
        auto pdu_def = endpoint_->get_pdu_definition();
        if (pdu_def == nullptr) {
            std::cerr << "ERROR: PDU Definition is not available in the endpoint." << std::endl;
            return false;
        }

        for (const auto& client : service_config["clients"]) {
            std::string client_name = client["name"];
            // Register client
            registered_clients_.push_back(client_name);
            // Initialize server state
            server_states_[client_name].state = SERVER_STATE_IDLE;
            server_states_[client_name].request_id = 0;

            // Request PDU
            PduDef req_def;
            req_def.org_name = client_name + "Req";
            req_def.name = service_name + "_" + req_def.org_name;
            req_def.channel_id = client["requestChannelId"];
            req_def.pdu_size = service_config["pduSize"]["server"]["baseSize"].get<size_t>() 
                + service_config["pduSize"]["client"]["heapSize"].get<size_t>()
                + pdu_meta_data_size;
            req_def.method_type = "RPC";
            pdu_def->add_definition(service_name, req_def);

            // Response PDU
            PduDef res_def;
            res_def.org_name = client_name + "Res";
            res_def.name = service_name + "_" + res_def.org_name;
            res_def.channel_id = client["responseChannelId"];
            res_def.pdu_size = service_config["pduSize"]["client"]["baseSize"].get<size_t>() 
                + service_config["pduSize"]["server"]["heapSize"].get<size_t>()
                + pdu_meta_data_size;
            res_def.method_type = "RPC";
            pdu_def->add_definition(service_name, res_def);
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse service config: " << e.what() << std::endl;
        return false;
    }

    return true;
}

void RpcServerEndpointImpl::pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) 
{
    std::cout << "INFO: Received PDU for service: " << resolved_pdu_key.robot << std::endl;
    std::cout << " DEBUG: instance count: " << instances_.size() << std::endl;
    for (const auto& instance : instances_) {
        // robot_name = service_name
        if (instance->get_service_name() != resolved_pdu_key.robot) {
            std::cout << "INFO: Skipping PDU for service: " << resolved_pdu_key.robot << ", does not match instance service: " << instance->get_service_name() << std::endl;
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
        std::cout << "INFO: PDU stored for service: " << resolved_pdu_key.robot << std::endl;
        return;
    }
    std::cerr << "WARNING: Received PDU for unknown service: " << resolved_pdu_key.robot << std::endl;
}


ServerEventType RpcServerEndpointImpl::poll(RpcRequest& request) 
{
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (pending_requests_.empty()) {
        //std::cout << "INFO: No pending requests to process. : service_name=" << service_name_ << std::endl;
        return ServerEventType::NONE;
    }
    PendingRequest pending_request = std::move(pending_requests_.front());
    pending_requests_.erase(pending_requests_.begin());
    request.pdu = std::move(pending_request.pdu_data);

    convertor_request_.pdu2cpp(reinterpret_cast<char*>(request.pdu.data()), request.header);
    if (!validate_header(request.header)) {
        std::cerr << "ERROR: Invalid request header received and ignored" << std::endl;
        //ignore invalid request
        send_error_reply(request.header, HAKO_SERVICE_RESULT_CODE_ERROR);
        return ServerEventType::NONE;
    }
    // Check if client_name exists in server_states_ before accessing
    if (server_states_.count(request.header.client_name) == 0) {
        std::cerr << "ERROR: Unknown client_name in request header: " << request.header.client_name << std::endl;
        send_error_reply(request.header, HAKO_SERVICE_RESULT_CODE_INVALID);
        return ServerEventType::NONE;
    }

    if (request.header.opcode == HAKO_SERVICE_OPERATION_CODE_CANCEL) {
        std::cout << "INFO: Received cancel request for client: " << request.header.client_name << std::endl;
        return handle_cancel_request(request);
    }
    else { // REQUEST
        std::cout << "INFO: Received request for client: " << request.header.client_name << std::endl;
        return handle_request_in(request);
    }
}

void RpcServerEndpointImpl::send_reply(std::string client_name, const PduData& pdu) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (server_states_.find(client_name) == server_states_.end()) {
        std::cerr << "ERROR: Unknown client_name: " << client_name << std::endl;
        return;
    }
    else if (server_states_[client_name].state == ServerState::SERVER_STATE_IDLE) {
        std::cerr << "ERROR: Cannot send reply, server state is IDLE for client: " << client_name << std::endl;
        return;
    }
    
    server_states_[client_name].state = ServerState::SERVER_STATE_IDLE;
    server_states_[client_name].request_id = 0;
    std::cout << "INFO: Reset state to IDLE for client: " << client_name << std::endl;

    hakoniwa::pdu::PduKey pdu_key = {service_name_, client_name + "Res"};
    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(pdu.data()), pdu.size());
    auto error = endpoint_->send(pdu_key,  data);
    if (error != HAKO_PDU_ERR_OK) {
        std::cerr << "ERROR: Failed to send reply to client_name: " << client_name << ", error: " << static_cast<int>(error) << std::endl;
    }
    else {
        std::cout << "INFO: Sent reply to client_name: " << client_name << std::endl;
    }
}

void RpcServerEndpointImpl::send_cancel_reply(std::string client_name, const PduData& pdu) {
    // Similar to send_reply, but with a CANCELED result code
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (server_states_.find(client_name) == server_states_.end()) {
        std::cerr << "ERROR: Unknown client_name: " << client_name << std::endl;
        return;
    }
    else if (server_states_[client_name].state != ServerState::SERVER_STATE_CANCELLING) {
        std::cerr << "ERROR: Cannot send reply, server state is not CANCELLING for client: " << client_name << std::endl;
        return;
    }
    
    // Reset server state to IDLE
    server_states_[client_name].state = ServerState::SERVER_STATE_IDLE;
    server_states_[client_name].request_id = 0;
    std::cout << "INFO: Reset state to IDLE for client: " << client_name << " after cancellation" << std::endl;

    hakoniwa::pdu::PduKey pdu_key = {service_name_, client_name + "Res"};
    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(pdu.data()), pdu.size());
    auto error = endpoint_->send(pdu_key,  data);
    if (error != HAKO_PDU_ERR_OK) {
        std::cerr << "ERROR: Failed to send reply to client_name: " << client_name << ", error: " << static_cast<int>(error) << std::endl;
    }
    std::cout << "INFO: Sent cancel reply to client_name: " << client_name << std::endl;
}

bool RpcServerEndpointImpl::validate_header(HakoCpp_ServiceRequestHeader& header)
{
    if (header.service_name != this->service_name_) {
        std::cerr << "ERROR: service_name is invalid: " << header.service_name << std::endl;
        return false;
    }
    if (std::find(registered_clients_.begin(), registered_clients_.end(), header.client_name) == registered_clients_.end()) {
        std::cerr << "ERROR: client_name is invalid: " << header.client_name << std::endl;
        return false;
    }
    if (header.opcode >= HakoServiceOperationCode::HAKO_SERVICE_OPERATION_NUM) {
        std::cerr << "ERROR: opcode is invalid: " << header.opcode << std::endl;
        return false;
    }
    return true;
} 


ServerEventType RpcServerEndpointImpl::handle_request_in(RpcRequest& request) {
    if (server_states_[request.header.client_name].state == ServerState::SERVER_STATE_IDLE) {
        std::cout << "INFO: Received request for client: " << request.header.client_name << std::endl;
        server_states_[request.header.client_name].state = ServerState::SERVER_STATE_RUNNING;
        server_states_[request.header.client_name].request_id = request.header.request_id;
        request.client_name = request.header.client_name;
        return ServerEventType::REQUEST_IN;
    }
    else if (server_states_[request.header.client_name].state == ServerState::SERVER_STATE_RUNNING) {
        std::cerr << "WARNING: Received request while previous request is still running for client: " << request.header.client_name << std::endl;
        send_error_reply(request.header, HAKO_SERVICE_RESULT_CODE_BUSY);
        return ServerEventType::NONE;
    }
    else { // CANCELING
        std::cerr << "WARNING: Received request while previous request is cancelling for client: " << request.header.client_name << std::endl;
        send_error_reply(request.header, HAKO_SERVICE_RESULT_CODE_BUSY);
        return ServerEventType::NONE;
    }
}

ServerEventType RpcServerEndpointImpl::handle_cancel_request(RpcRequest& request) {
    if (server_states_[request.header.client_name].state == ServerState::SERVER_STATE_RUNNING) {
        if (server_states_[request.header.client_name].request_id != request.header.request_id) {
            // Request ID does not match
            std::cerr << "WARNING: Received cancel request with mismatched request_id for client: " << request.header.client_name << std::endl;
            send_error_reply(request.header, HAKO_SERVICE_RESULT_CODE_INVALID);
            return ServerEventType::NONE;
        }
        server_states_[request.header.client_name].state = ServerState::SERVER_STATE_CANCELLING;
        std::cout << "INFO: Received cancel request for client: " << request.header.client_name << std::endl;
        return ServerEventType::REQUEST_CANCEL;
    }
    else if (server_states_[request.header.client_name].state == ServerState::SERVER_STATE_IDLE) {
        // Already idle, nothing to cancel
        // client must get normal reply and cancel request must be ignored
        std::cerr << "WARNING: Received cancel request while idle for client: " << request.header.client_name << std::endl;
        return ServerEventType::NONE;
    }
    else {
        // Already cancelling
        //TODO reply busy reply
        std::cerr << "WARNING: Received cancel request while already cancelling for client: " << request.header.client_name << std::endl;
        send_error_reply(request.header, HAKO_SERVICE_RESULT_CODE_BUSY);
        return ServerEventType::NONE;
    }
}

void RpcServerEndpointImpl::clear_pending_requests()
{
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    pending_requests_.clear();
}

} // namespace hakoniwa::pdu::rpc
