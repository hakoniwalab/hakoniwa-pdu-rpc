#include "hakoniwa/pdu/rpc/rpc_client_endpoint_impl.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace hakoniwa::pdu::rpc {

std::vector<std::shared_ptr<RpcClientEndpointImpl>> RpcClientEndpointImpl::instances_;

RpcClientEndpointImpl::RpcClientEndpointImpl(
    const std::string& service_name, const std::string& client_name, uint64_t delta_time_usec,
    const std::shared_ptr<hakoniwa::pdu::Endpoint>& endpoint, std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source)
 : IRpcClientEndpoint(service_name, client_name, delta_time_usec),
      endpoint_(endpoint), time_source_(time_source), 
      current_timeout_usec_(0), request_start_time_usec_(0) {
    
    client_state_.state = CLIENT_STATE_IDLE;
    client_state_.request_id = 0;

    if (endpoint_) {
        endpoint_->set_on_recv_callback([](const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) {
            RpcClientEndpointImpl::pdu_recv_callback(resolved_pdu_key, data);
        });
    }
}


RpcClientEndpointImpl::~RpcClientEndpointImpl() {
    auto it = std::remove_if(instances_.begin(), instances_.end(),
        [this](const std::shared_ptr<RpcClientEndpointImpl>& p) {
            return p.get() == this;
        });
    instances_.erase(it, instances_.end());
}

bool RpcClientEndpointImpl::initialize(const nlohmann::json& service_config, int pdu_meta_data_size) {
    if (!endpoint_) {
        std::cerr << "ERROR: Endpoint is not initialized." << std::endl;
        return false;
    }
    instances_.push_back(shared_from_this());

    try {
        std::string service_name_str = service_config["name"];
        if (service_name_str != this->service_name_) {
            std::cerr << "ERROR: Service name mismatch in config." << std::endl;
            return false;
        }

        auto& pdu_def = endpoint_->get_pdu_definition();

        bool client_found = false;
        for (const auto& client : service_config["clients"]) {
            std::string client_name_str = client["name"];
            if (client_name_str == this->client_name_) {
                client_found = true;
                // Request PDU
                PduDef req_def;
                req_def.org_name = client_name_ + "Req";
                req_def.name = service_name_ + "_" + req_def.org_name;
                req_def.channel_id = client["requestChannelId"];
                req_def.pdu_size = service_config["pduSize"]["server"]["baseSize"].get<size_t>() 
                    + service_config["pduSize"]["client"]["heapSize"].get<size_t>()
                    + pdu_meta_data_size;
                req_def.method_type = "RPC";
                pdu_def.add_definition(service_name_, req_def);

                // Response PDU
                PduDef res_def;
                res_def.org_name = client_name_ + "Res";
                res_def.name = service_name_ + "_" + res_def.org_name;
                res_def.channel_id = client["responseChannelId"];
                res_def.pdu_size = service_config["pduSize"]["client"]["baseSize"].get<size_t>() 
                    + service_config["pduSize"]["server"]["heapSize"].get<size_t>()
                    + pdu_meta_data_size;
                res_def.method_type = "RPC";
                pdu_def.add_definition(service_name_, res_def);
                break;
            }
        }
        if (!client_found) {
            std::cerr << "ERROR: Client '" << this->client_name_ << "' not found in service config." << std::endl;
            return false;
        }

    } catch (const nlohmann::json::exception& e) {
        std::cerr << "ERROR: Failed to parse service config: " << e.what() << std::endl;
        return false;
    }
    return true;
}

void RpcClientEndpointImpl::pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) {
    for (auto& instance : instances_) {
        if (instance->get_service_name() == resolved_pdu_key.robot) {
            std::string expected_pdu_name = instance->get_client_name() + "Res";
            if (instance->endpoint_->get_pdu_name(resolved_pdu_key) == expected_pdu_name) {
                PduData pdu_data;
                pdu_data.resize(data.size());
                std::memcpy(pdu_data.data(), data.data(), data.size());
                instance->put_pending_response({resolved_pdu_key.robot, expected_pdu_name}, pdu_data);
                return;
            }
        }
    }
    //std::cerr << "WARNING: Received PDU for unknown client or service: " << resolved_pdu_key.robot << std::endl;
}

bool RpcClientEndpointImpl::send_request(const PduData& pdu) {
    hakoniwa::pdu::PduKey pdu_key = {service_name_, client_name_ + "Req"};
    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(pdu.data()), pdu.size());
    auto err = endpoint_->send(pdu_key, data);
    if (err != HAKO_PDU_ERR_OK) {
        std::cerr << "ERROR: Failed to send request PDU: error=" << static_cast<int>(err) << std::endl;
        return false;
    }
    return true;
}

bool RpcClientEndpointImpl::call(const PduData& pdu, uint64_t timeout_usec) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (client_state_.state != CLIENT_STATE_IDLE) {
        std::cerr << "ERROR: Client is busy" << std::endl;
        return false;
    }
    client_state_.state = CLIENT_STATE_RUNNING;
    client_state_.request_id = this->current_request_id_;
    this->current_timeout_usec_ = timeout_usec;
    this->request_start_time_usec_ = time_source_->get_microseconds();

    // Check if send_request fails
    if (!send_request(pdu)) {
        std::cerr << "ERROR: send_request failed for RPC call." << std::endl;
        client_state_.state = CLIENT_STATE_IDLE; // Rollback state
        return false;
    }
    //std::cout << "INFO: Sent request with request_id: " << client_state_.request_id << std::endl;

    return true;
}

ClientEventType RpcClientEndpointImpl::poll(RpcResponse& response) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (client_state_.state == CLIENT_STATE_IDLE) {
        return ClientEventType::NONE;
    }

    if (client_state_.state == CLIENT_STATE_RUNNING || client_state_.state == CLIENT_STATE_CANCELLING) {
        // Timeout check
        if (current_timeout_usec_ > 0) {
            if (time_source_->get_microseconds() - request_start_time_usec_ > current_timeout_usec_) {
                std::cerr << "ERROR: RPC call timed out" << std::endl;
                if (send_cancel_request()) {
                    client_state_.state = CLIENT_STATE_CANCELLING;
                    std::cout << "INFO: Sent cancel request due to timeout." << std::endl;
                } else {
                    client_state_.state = CLIENT_STATE_IDLE;
                    std::cerr << "ERROR: Failed to send cancel request after timeout." << std::endl;
                }
                return ClientEventType::RESPONSE_TIMEOUT;
            }
        }

        // Response check
        auto it = pending_responses_.begin();
        while (it != pending_responses_.end()) {
            HakoCpp_ServiceResponseHeader response_header;
            convertor_response_.pdu2cpp(reinterpret_cast<char*>(it->pdu_data.data()), response_header);
            if (response_header.request_id == client_state_.request_id) {
                response.pdu = std::move(it->pdu_data);
                response.header = response_header;
                pending_responses_.erase(it); // Remove the PDU from the queue BEFORE calling handle_response_in

                return handle_response_in(response);
            }
            ++it;
        }
    }
    // Add handling for CANCELLING state if needed
    return ClientEventType::NONE;
}


bool RpcClientEndpointImpl::validate_header(HakoCpp_ServiceResponseHeader& header)
{
    // Assuming lock is already held by poll()
    if (header.service_name != this->service_name_) {
        std::cerr << "ERROR: service_name is invalid: " << header.service_name << std::endl;
        return false;
    }
    if (header.client_name != this->client_name_) {
        std::cerr << "ERROR: client_name is invalid: " << header.client_name << std::endl;
        return false;
    }
    if (header.request_id != this->client_state_.request_id) {
        std::cerr << "ERROR: request_id is invalid: " << header.request_id << std::endl;
        return false;
    }
    if (header.result_code >= HakoServiceResultCode::HAKO_SERVICE_RESULT_CODE_NUM) {
        std::cerr << "ERROR: result_code is invalid: " << header.result_code << std::endl;
        return false;
    }
    return true;
}

ClientEventType RpcClientEndpointImpl::handle_response_in(RpcResponse& response)
{
    // The lock is already held by poll()
    if (!validate_header(response.header)) {
        std::cerr << "ERROR: Invalid response header during processing" << std::endl;
        client_state_.state = CLIENT_STATE_IDLE; // Invalidate current request
        return ClientEventType::NONE; // Or a dedicated error event
    }
    
    switch (response.header.result_code) {
        case HAKO_SERVICE_RESULT_CODE_OK:
            client_state_.state = CLIENT_STATE_IDLE;
            return ClientEventType::RESPONSE_IN;
        case HAKO_SERVICE_RESULT_CODE_CANCELED:
            return handle_cancel_response(response);
        default:
            std::cerr << "ERROR: RPC call failed with error code: " << response.header.result_code << std::endl;
            client_state_.state = CLIENT_STATE_IDLE;
            return ClientEventType::NONE;
    }
}

ClientEventType RpcClientEndpointImpl::handle_cancel_response(RpcResponse& response)
{
    (void)response; // response might be used for logging in the future
    std::cout << "INFO: RPC request " << client_state_.request_id << " was successfully cancelled." << std::endl;
    client_state_.state = CLIENT_STATE_IDLE;
    return ClientEventType::RESPONSE_CANCEL;
}

void RpcClientEndpointImpl::clear_all_instances()
{
    instances_.clear();
}

void RpcClientEndpointImpl::clear_pending_responses()
{
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    pending_responses_.clear();
}

} // namespace hakoniwa::pdu::rpc