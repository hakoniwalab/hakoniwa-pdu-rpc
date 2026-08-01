#include "hakoniwa/pdu/rpc/c_rpc.h"

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
using hakoniwa::pdu::EndpointContainer;
using hakoniwa::pdu::rpc::ClientEventType;
using hakoniwa::pdu::rpc::PduData;
using hakoniwa::pdu::rpc::RpcRequest;
using hakoniwa::pdu::rpc::RpcResponse;
using hakoniwa::pdu::rpc::RpcServicesClient;
using hakoniwa::pdu::rpc::RpcServicesServer;
using hakoniwa::pdu::rpc::ServerEventType;

constexpr const char* kClientImpl = "RpcClientEndpointImpl";
constexpr const char* kServerImpl = "RpcServerEndpointImpl";
constexpr const char* kDefaultTimeSource = "real";

bool valid_text(const char* value) {
    return value != nullptr && value[0] != '\0';
}

void copy_name(char* destination, size_t capacity, const std::string& source) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const size_t count = std::min(capacity - 1, source.size());
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

hako_pdu_rpc_error_t copy_pdu(
    const PduData& source,
    uint8_t* destination,
    size_t capacity,
    size_t* out_size)
{
    if (out_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    *out_size = source.size();
    if (source.size() > capacity || (source.size() > 0 && destination == nullptr)) {
        return HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL;
    }
    if (!source.empty()) {
        std::memcpy(destination, source.data(), source.size());
    }
    return HAKO_PDU_RPC_OK;
}

PduData make_pdu(const uint8_t* data, size_t size) {
    if (size == 0) {
        return {};
    }
    return PduData(data, data + size);
}

hako_pdu_rpc_client_event_t map_client_event(ClientEventType event) {
    switch (event) {
    case ClientEventType::RESPONSE_IN:
        return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN;
    case ClientEventType::RESPONSE_CANCEL:
        return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL;
    case ClientEventType::RESPONSE_TIMEOUT:
        return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT;
    case ClientEventType::NONE:
    default:
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
}

hako_pdu_rpc_server_event_t map_server_event(ServerEventType event) {
    switch (event) {
    case ServerEventType::REQUEST_IN:
        return HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN;
    case ServerEventType::REQUEST_CANCEL:
        return HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL;
    case ServerEventType::NONE:
    default:
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
}
} // namespace

struct hako_pdu_rpc_client_handle {
    std::string node_id;
    std::string client_name;
    std::string service_config_path;
    std::string endpoint_config_path;
    uint64_t delta_time_usec{1000};
    std::string time_source_type{kDefaultTimeSource};
    std::shared_ptr<EndpointContainer> endpoints;
    std::unique_ptr<RpcServicesClient> rpc;
    bool started{false};
};

struct hako_pdu_rpc_server_handle {
    std::string node_id;
    std::string service_config_path;
    std::string endpoint_config_path;
    uint64_t delta_time_usec{1000};
    std::string time_source_type{kDefaultTimeSource};
    std::shared_ptr<EndpointContainer> endpoints;
    std::unique_ptr<RpcServicesServer> rpc;
    std::mutex pending_mutex;
    uint64_t next_token{1};
    std::map<uint64_t, RpcRequest> pending_requests;
    bool started{false};
};

extern "C" {

hako_pdu_rpc_client_handle_t* hako_pdu_rpc_client_create(
    const char* node_id,
    const char* client_name,
    const char* service_config_path,
    const char* endpoint_config_path,
    uint64_t delta_time_usec,
    const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(client_name) ||
        !valid_text(service_config_path) || !valid_text(endpoint_config_path)) {
        return nullptr;
    }
    try {
        auto* handle = new hako_pdu_rpc_client_handle_t();
        handle->node_id = node_id;
        handle->client_name = client_name;
        handle->service_config_path = service_config_path;
        handle->endpoint_config_path = endpoint_config_path;
        handle->delta_time_usec = delta_time_usec;
        handle->time_source_type = valid_text(time_source_type) ? time_source_type : kDefaultTimeSource;
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void hako_pdu_rpc_client_destroy(hako_pdu_rpc_client_handle_t* handle) {
    if (handle == nullptr) {
        return;
    }
    (void)hako_pdu_rpc_client_stop(handle);
    delete handle;
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_start(hako_pdu_rpc_client_handle_t* handle) {
    if (handle == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return HAKO_PDU_RPC_OK;
    }
    try {
        handle->endpoints = std::make_shared<EndpointContainer>(handle->node_id, handle->endpoint_config_path);
        if (handle->endpoints->initialize() != HAKO_PDU_ERR_OK) {
            handle->endpoints.reset();
            return HAKO_PDU_RPC_ERROR_INITIALIZE;
        }
        if (handle->endpoints->start_all() != HAKO_PDU_ERR_OK) {
            handle->endpoints.reset();
            return HAKO_PDU_RPC_ERROR_START;
        }
        handle->rpc = std::make_unique<RpcServicesClient>(
            handle->node_id,
            handle->client_name,
            handle->service_config_path,
            kClientImpl,
            handle->delta_time_usec,
            handle->time_source_type);
        if (!handle->rpc->initialize_services(handle->endpoints) || !handle->rpc->start_all_services()) {
            handle->rpc.reset();
            (void)handle->endpoints->stop_all();
            handle->endpoints.reset();
            return HAKO_PDU_RPC_ERROR_INITIALIZE;
        }
        handle->started = true;
        return HAKO_PDU_RPC_OK;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_stop(hako_pdu_rpc_client_handle_t* handle) {
    if (handle == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (handle->rpc) {
        handle->rpc->stop_all_services();
        handle->rpc->clear_all_instances();
        handle->rpc.reset();
    }
    if (handle->endpoints) {
        (void)handle->endpoints->stop_all();
        handle->endpoints.reset();
    }
    handle->started = false;
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_create_request_buffer(
    hako_pdu_rpc_client_handle_t* handle,
    const char* service_name,
    uint8_t* request_buffer,
    size_t request_capacity,
    size_t* out_request_size)
{
    if (handle == nullptr || !valid_text(service_name) || out_request_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }
    PduData pdu;
    if (!handle->rpc->create_request_buffer(service_name, pdu)) {
        return HAKO_PDU_RPC_ERROR_NOT_FOUND;
    }
    return copy_pdu(pdu, request_buffer, request_capacity, out_request_size);
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_call(
    hako_pdu_rpc_client_handle_t* handle,
    const char* service_name,
    const uint8_t* request_pdu,
    size_t request_pdu_size,
    uint64_t timeout_usec)
{
    if (handle == nullptr || !valid_text(service_name) ||
        (request_pdu_size > 0 && request_pdu == nullptr)) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }
    return handle->rpc->call(service_name, make_pdu(request_pdu, request_pdu_size), timeout_usec)
        ? HAKO_PDU_RPC_OK
        : HAKO_PDU_RPC_ERROR_CALL;
}

hako_pdu_rpc_client_event_t hako_pdu_rpc_client_poll(
    hako_pdu_rpc_client_handle_t* handle,
    hako_pdu_rpc_response_info_t* out_info,
    uint8_t* response_buffer,
    size_t response_capacity,
    size_t* out_response_size,
    hako_pdu_rpc_error_t* out_error)
{
    if (out_error) {
        *out_error = HAKO_PDU_RPC_OK;
    }
    if (handle == nullptr || out_info == nullptr || out_response_size == nullptr) {
        if (out_error) *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
    if (!handle->started || !handle->rpc) {
        if (out_error) *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING;
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
    std::string service_name;
    RpcResponse response;
    const ClientEventType event = handle->rpc->poll(service_name, response);
    if (event == ClientEventType::NONE) {
        *out_response_size = 0;
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
    copy_name(out_info->service_name, sizeof(out_info->service_name), service_name);
    out_info->pdu_size = response.pdu.size();
    const auto copy_result = copy_pdu(response.pdu, response_buffer, response_capacity, out_response_size);
    if (copy_result != HAKO_PDU_RPC_OK) {
        if (out_error) *out_error = copy_result;
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
    return map_client_event(event);
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_cancel(
    hako_pdu_rpc_client_handle_t* handle,
    const char* service_name)
{
    if (handle == nullptr || !valid_text(service_name)) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }
    return handle->rpc->send_cancel_request(service_name)
        ? HAKO_PDU_RPC_OK
        : HAKO_PDU_RPC_ERROR_CALL;
}

hako_pdu_rpc_server_handle_t* hako_pdu_rpc_server_create(
    const char* node_id,
    const char* service_config_path,
    const char* endpoint_config_path,
    uint64_t delta_time_usec,
    const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(service_config_path) || !valid_text(endpoint_config_path)) {
        return nullptr;
    }
    try {
        auto* handle = new hako_pdu_rpc_server_handle_t();
        handle->node_id = node_id;
        handle->service_config_path = service_config_path;
        handle->endpoint_config_path = endpoint_config_path;
        handle->delta_time_usec = delta_time_usec;
        handle->time_source_type = valid_text(time_source_type) ? time_source_type : kDefaultTimeSource;
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void hako_pdu_rpc_server_destroy(hako_pdu_rpc_server_handle_t* handle) {
    if (handle == nullptr) {
        return;
    }
    (void)hako_pdu_rpc_server_stop(handle);
    delete handle;
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_start(hako_pdu_rpc_server_handle_t* handle) {
    if (handle == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return HAKO_PDU_RPC_OK;
    }
    try {
        handle->endpoints = std::make_shared<EndpointContainer>(handle->node_id, handle->endpoint_config_path);
        if (handle->endpoints->initialize() != HAKO_PDU_ERR_OK) {
            handle->endpoints.reset();
            return HAKO_PDU_RPC_ERROR_INITIALIZE;
        }
        if (handle->endpoints->start_all() != HAKO_PDU_ERR_OK) {
            handle->endpoints.reset();
            return HAKO_PDU_RPC_ERROR_START;
        }
        handle->rpc = std::make_unique<RpcServicesServer>(
            handle->node_id,
            kServerImpl,
            handle->service_config_path,
            handle->delta_time_usec,
            handle->time_source_type);
        if (!handle->rpc->initialize_services(handle->endpoints) || !handle->rpc->start_all_services()) {
            handle->rpc.reset();
            (void)handle->endpoints->stop_all();
            handle->endpoints.reset();
            return HAKO_PDU_RPC_ERROR_INITIALIZE;
        }
        handle->started = true;
        return HAKO_PDU_RPC_OK;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_stop(hako_pdu_rpc_server_handle_t* handle) {
    if (handle == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (handle->rpc) {
        handle->rpc->stop_all_services();
        handle->rpc->clear_all_instances();
        handle->rpc.reset();
    }
    if (handle->endpoints) {
        (void)handle->endpoints->stop_all();
        handle->endpoints.reset();
    }
    {
        std::lock_guard<std::mutex> lock(handle->pending_mutex);
        handle->pending_requests.clear();
    }
    handle->started = false;
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_server_event_t hako_pdu_rpc_server_poll(
    hako_pdu_rpc_server_handle_t* handle,
    hako_pdu_rpc_request_info_t* out_info,
    uint8_t* request_buffer,
    size_t request_capacity,
    size_t* out_request_size,
    hako_pdu_rpc_error_t* out_error)
{
    if (out_error) {
        *out_error = HAKO_PDU_RPC_OK;
    }
    if (handle == nullptr || out_info == nullptr || out_request_size == nullptr) {
        if (out_error) *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->rpc) {
        if (out_error) *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING;
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
    RpcRequest request;
    const ServerEventType event = handle->rpc->poll(request);
    if (event == ServerEventType::NONE) {
        *out_request_size = 0;
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
    const auto copy_result = copy_pdu(request.pdu, request_buffer, request_capacity, out_request_size);
    if (copy_result != HAKO_PDU_RPC_OK) {
        if (out_error) *out_error = copy_result;
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(handle->pending_mutex);
        token = handle->next_token++;
        handle->pending_requests.emplace(token, request);
    }
    out_info->request_token = token;
    copy_name(out_info->service_name, sizeof(out_info->service_name), request.header.service_name);
    copy_name(out_info->client_name, sizeof(out_info->client_name), request.client_name);
    out_info->pdu_size = request.pdu.size();
    return map_server_event(event);
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_create_reply_buffer(
    hako_pdu_rpc_server_handle_t* handle,
    uint64_t request_token,
    uint8_t status,
    int32_t result_code,
    uint8_t* response_buffer,
    size_t response_capacity,
    size_t* out_response_size)
{
    if (handle == nullptr || out_response_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }
    RpcRequest request;
    {
        std::lock_guard<std::mutex> lock(handle->pending_mutex);
        const auto it = handle->pending_requests.find(request_token);
        if (it == handle->pending_requests.end()) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        request = it->second;
    }
    PduData response;
    handle->rpc->create_reply_buffer(
        request.header,
        static_cast<Hako_uint8>(status),
        static_cast<Hako_int32>(result_code),
        response);
    return copy_pdu(response, response_buffer, response_capacity, out_response_size);
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_send_reply(
    hako_pdu_rpc_server_handle_t* handle,
    uint64_t request_token,
    const uint8_t* response_pdu,
    size_t response_pdu_size)
{
    if (handle == nullptr || (response_pdu_size > 0 && response_pdu == nullptr)) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }
    RpcRequest request;
    {
        std::lock_guard<std::mutex> lock(handle->pending_mutex);
        const auto it = handle->pending_requests.find(request_token);
        if (it == handle->pending_requests.end()) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        request = it->second;
        handle->pending_requests.erase(it);
    }
    handle->rpc->send_reply(request.header, make_pdu(response_pdu, response_pdu_size));
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_send_cancel_reply(
    hako_pdu_rpc_server_handle_t* handle,
    uint64_t request_token,
    const uint8_t* response_pdu,
    size_t response_pdu_size)
{
    if (handle == nullptr || (response_pdu_size > 0 && response_pdu == nullptr)) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }
    RpcRequest request;
    {
        std::lock_guard<std::mutex> lock(handle->pending_mutex);
        const auto it = handle->pending_requests.find(request_token);
        if (it == handle->pending_requests.end()) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        request = it->second;
        handle->pending_requests.erase(it);
    }
    handle->rpc->send_cancel_reply(request.header, make_pdu(response_pdu, response_pdu_size));
    return HAKO_PDU_RPC_OK;
}

} // extern "C"
