#include "hakoniwa/pdu/rpc/pdu_rpc_client_endpoint_impl.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace hakoniwa::pdu::rpc {

std::vector<std::shared_ptr<PduRpcClientEndpointImpl>> PduRpcClientEndpointImpl::instances_;

PduRpcClientEndpointImpl::PduRpcClientEndpointImpl(
    const std::string& service_name, const std::string& client_name, uint64_t delta_time_usec,
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint, std::shared_ptr<ITimeSource> time_source)
    : IPduRpcClientEndpoint(service_name, client_name, delta_time_usec),
      endpoint_(endpoint), time_source_(time_source) {
    
    client_state_.state = CLIENT_STATE_IDLE;
    client_state_.request_id = 0;

    if (endpoint_) {
        endpoint_->set_on_recv_callback([](const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) {
            PduRpcClientEndpointImpl::pdu_recv_callback(resolved_pdu_key, data);
        });
    }
}

bool PduRpcClientEndpointImpl::initialize(const nlohmann::json& service_config) {
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
                req_def.pdu_size = service_config["pduSize"]["client"]["baseSize"].get<size_t>() + service_config["pduSize"]["client"]["heapSize"].get<size_t>();
                req_def.method_type = "RPC";
                pdu_def.add_definition(service_name_, req_def);

                // Response PDU
                PduDef res_def;
                res_def.org_name = client_name_ + "Res";
                res_def.name = service_name_ + "_" + res_def.org_name;
                res_def.channel_id = client["responseChannelId"];
                res_def.pdu_size = service_config["pduSize"]["server"]["baseSize"].get<size_t>() + service_config["pduSize"]["server"]["heapSize"].get<size_t>();
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

void PduRpcClientEndpointImpl::pdu_recv_callback(const hakoniwa::pdu::PduResolvedKey& resolved_pdu_key, std::span<const std::byte> data) {
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

void PduRpcClientEndpointImpl::send_request(const PduData& pdu) {
    hakoniwa::pdu::PduKey pdu_key = {service_name_, client_name_ + "Req"};
    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(pdu.data()), pdu.size());
    auto err = endpoint_->send(pdu_key, data);
    if (err != HAKO_PDU_ERR_OK) {
        throw std::runtime_error("Failed to send request PDU: error=" + std::to_string(err));
    }
}


std::future<PduData> PduRpcClientEndpointImpl::call(const PduData& pdu, uint64_t timeout_usec) {
    std::promise<PduData> promise;
    //HakoCpp_ServiceRequestHeader request_header;
    //convertor_request_.pdu2cpp(reinterpret_cast<const char*>(pdu.data()), request_header);

    {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        if (client_state_.state != CLIENT_STATE_IDLE) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error("Client is busy")));
            return promise.get_future();
        }
        client_state_.state = CLIENT_STATE_RUNNING;
        client_state_.request_id = this->current_request_id_;
    }

    try {
        send_request(pdu);
    } catch (const std::exception& e) {
        promise.set_exception(std::make_exception_ptr(e));
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        client_state_.state = CLIENT_STATE_IDLE;
        return promise.get_future();
    }

    auto start_time = time_source_->get_current_time_usec();
    while (true) {
        if (time_source_->get_current_time_usec() - start_time > timeout_usec) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error("RPC call timed out")));
            {
                std::lock_guard<std::recursive_mutex> lock(mtx_);
                client_state_.state = CLIENT_STATE_IDLE;
            }
            break;
        }

        RpcResponse response;
        bool response_found = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            auto it = pending_responses_.begin();
            while (it != pending_responses_.end()) {
                HakoCpp_ServiceResponseHeader response_header;
                convertor_response_.pdu2cpp(reinterpret_cast<char*>(it->pdu_data.data()), response_header);
                if (response_header.request_id == client_state_.request_id) {
                    response.pdu = std::move(it->pdu_data);
                    response.header = response_header;
                    pending_responses_.erase(it);
                    response_found = true;
                    break;
                }
                ++it;
            }
        }

        if (response_found) {
            if (!validate_header(response.header)) {
                promise.set_exception(std::make_exception_ptr(std::runtime_error("Invalid response header")));
            } else if (response.header.result_code == HAKO_SERVICE_RESULT_CODE_OK) {
                promise.set_value(response.pdu);
            } else {
                promise.set_exception(std::make_exception_ptr(std::runtime_error("RPC call failed with error code: " + std::to_string(response.header.result_code))));
            }
            
            {
                std::lock_guard<std::recursive_mutex> lock(mtx_);
                client_state_.state = CLIENT_STATE_IDLE;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(delta_time_usec_));
    }

    return promise.get_future();
}


bool PduRpcClientEndpointImpl::validate_header(HakoCpp_ServiceResponseHeader& header)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_);
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
    if (header.result_code >= HakoServiceResultCodeType::HAKO_SERVICE_RESULT_CODE_NUM) {
        std::cerr << "ERROR: result_code is invalid: " << header.result_code << std::endl;
        return false;
    }
    return true;
}

ClientEventType PduRpcClientEndpointImpl::handle_response_in(RpcResponse& response)
{
    // This logic is now integrated into the `call` method.
    // This function can be used for more complex state transitions if needed in the future.
    (void)response;
    return ClientEventType::RESPONSE_IN;
}

ClientEventType PduRpcClientEndpointImpl::handle_cancel_response(RpcResponse& response)
{
    // This logic is not fully implemented as cancel flow is not complete.
    (void)response;
    return ClientEventType::NONE;
}

} // namespace hakoniwa::pdu::rpc