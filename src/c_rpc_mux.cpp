#include "hakoniwa/pdu/rpc/c_rpc.h"

#include "hakoniwa/pdu/rpc/rpc_services_mux_server.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using hakoniwa::pdu::rpc::PduData;
using hakoniwa::pdu::rpc::RpcMuxRequest;
using hakoniwa::pdu::rpc::RpcServicesMuxServer;
using hakoniwa::pdu::rpc::ServerEventType;

namespace {

constexpr const char* kServerImpl = "RpcServerEndpointImpl";
constexpr const char* kDefaultTimeSource = "real";

bool valid_text(const char* value)
{
    return value != nullptr && value[0] != '\0';
}

void copy_name(char* dst, size_t capacity, const std::string& src)
{
    if (dst == nullptr || capacity == 0) {
        return;
    }
    const size_t count = std::min(capacity - 1, src.size());
    std::memcpy(dst, src.data(), count);
    dst[count] = '\0';
}

PduData make_pdu(const uint8_t* data, size_t size)
{
    return size == 0 ? PduData{} : PduData(data, data + size);
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
    if (source.size() > capacity || (!source.empty() && destination == nullptr)) {
        return HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL;
    }
    if (!source.empty()) {
        std::memcpy(destination, source.data(), source.size());
    }
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_error_t allocate_pdu(
    const PduData& source,
    uint8_t** out_buffer,
    size_t* out_size)
{
    if (out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    *out_buffer = nullptr;
    *out_size = 0;
    if (source.empty()) {
        return HAKO_PDU_RPC_OK;
    }

    auto* buffer = static_cast<uint8_t*>(std::malloc(source.size()));
    if (buffer == nullptr) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
    std::memcpy(buffer, source.data(), source.size());
    *out_buffer = buffer;
    *out_size = source.size();
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_server_event_t map_server_event(ServerEventType event)
{
    switch (event) {
    case ServerEventType::REQUEST_IN:
        return HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN;
    case ServerEventType::REQUEST_CANCEL:
        return HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL;
    default:
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
}

} // namespace

struct hako_pdu_rpc_mux_server_handle {
    std::unique_ptr<RpcServicesMuxServer> rpc;
    std::mutex pending_mutex;
    uint64_t next_token{1};
    std::map<uint64_t, RpcMuxRequest> pending_requests;
    bool started{false};
};

namespace {

hako_pdu_rpc_error_t store_request(
    hako_pdu_rpc_mux_server_handle_t* handle,
    RpcMuxRequest request,
    hako_pdu_rpc_request_info_t* info)
{
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(handle->pending_mutex);
        token = handle->next_token++;
        handle->pending_requests.emplace(token, request);
    }
    info->request_token = token;
    copy_name(
        info->service_name,
        sizeof(info->service_name),
        request.request.header.service_name);
    copy_name(
        info->client_name,
        sizeof(info->client_name),
        request.request.client_name);
    info->pdu_size = request.request.pdu.size();
    return HAKO_PDU_RPC_OK;
}

bool find_request(
    hako_pdu_rpc_mux_server_handle_t* handle,
    uint64_t token,
    RpcMuxRequest& request)
{
    std::lock_guard<std::mutex> lock(handle->pending_mutex);
    const auto it = handle->pending_requests.find(token);
    if (it == handle->pending_requests.end()) {
        return false;
    }
    request = it->second;
    return true;
}

bool take_request(
    hako_pdu_rpc_mux_server_handle_t* handle,
    uint64_t token,
    RpcMuxRequest& request)
{
    std::lock_guard<std::mutex> lock(handle->pending_mutex);
    const auto it = handle->pending_requests.find(token);
    if (it == handle->pending_requests.end()) {
        return false;
    }
    request = it->second;
    handle->pending_requests.erase(it);
    return true;
}

void reset_output(
    uint8_t** out_buffer,
    size_t* out_size,
    hako_pdu_rpc_error_t* out_error)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_RPC_OK;
    }
}

} // namespace

extern "C" {

hako_pdu_rpc_mux_server_handle_t* hako_pdu_rpc_mux_server_create(
    const char* node_id,
    const char* service_config_path,
    const char* endpoint_mux_config_path,
    uint64_t delta_time_usec,
    const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(service_config_path)
        || !valid_text(endpoint_mux_config_path) || delta_time_usec == 0) {
        return nullptr;
    }

    try {
        auto handle = std::make_unique<hako_pdu_rpc_mux_server_handle_t>();
        handle->rpc = std::make_unique<RpcServicesMuxServer>(
            node_id,
            kServerImpl,
            service_config_path,
            endpoint_mux_config_path,
            delta_time_usec,
            valid_text(time_source_type) ? time_source_type : kDefaultTimeSource);
        if (!handle->rpc->initialize()) {
            return nullptr;
        }
        return handle.release();
    } catch (...) {
        return nullptr;
    }
}

void hako_pdu_rpc_mux_server_destroy(hako_pdu_rpc_mux_server_handle_t* handle)
{
    if (handle == nullptr) {
        return;
    }
    (void)hako_pdu_rpc_mux_server_stop(handle);
    delete handle;
}

hako_pdu_rpc_error_t hako_pdu_rpc_mux_server_start(
    hako_pdu_rpc_mux_server_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return HAKO_PDU_RPC_OK;
    }
    if (!handle->rpc) {
        return HAKO_PDU_RPC_ERROR_INITIALIZE;
    }
    try {
        if (!handle->rpc->start()) {
            return HAKO_PDU_RPC_ERROR_START;
        }
        handle->started = true;
        return HAKO_PDU_RPC_OK;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_mux_server_stop(
    hako_pdu_rpc_mux_server_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    try {
        if (handle->rpc) {
            handle->rpc->stop();
        }
        {
            std::lock_guard<std::mutex> lock(handle->pending_mutex);
            handle->pending_requests.clear();
        }
        handle->rpc.reset();
        handle->started = false;
        return HAKO_PDU_RPC_OK;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_server_event_t hako_pdu_rpc_mux_server_poll(
    hako_pdu_rpc_mux_server_handle_t* handle,
    hako_pdu_rpc_request_info_t* info,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size,
    hako_pdu_rpc_error_t* out_error)
{
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_RPC_OK;
    }
    if (handle == nullptr || info == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->rpc) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }

    try {
        RpcMuxRequest request;
        const auto event = handle->rpc->poll(request);
        if (event == ServerEventType::NONE) {
            *out_size = 0;
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }
        const auto copy_result = copy_pdu(
            request.request.pdu, buffer, capacity, out_size);
        if (copy_result != HAKO_PDU_RPC_OK) {
            if (out_error != nullptr) {
                *out_error = copy_result;
            }
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }
        (void)store_request(handle, request, info);
        return map_server_event(event);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_INTERNAL;
        }
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
}

hako_pdu_rpc_server_event_t hako_pdu_rpc_mux_server_poll_alloc(
    hako_pdu_rpc_mux_server_handle_t* handle,
    hako_pdu_rpc_request_info_t* info,
    uint8_t** out_buffer,
    size_t* out_size,
    hako_pdu_rpc_error_t* out_error)
{
    reset_output(out_buffer, out_size, out_error);
    if (handle == nullptr || info == nullptr || out_buffer == nullptr
        || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->rpc) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }

    try {
        RpcMuxRequest request;
        const auto event = handle->rpc->poll(request);
        if (event == ServerEventType::NONE) {
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }
        const auto allocation = allocate_pdu(
            request.request.pdu, out_buffer, out_size);
        if (allocation != HAKO_PDU_RPC_OK) {
            if (out_error != nullptr) {
                *out_error = allocation;
            }
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }
        (void)store_request(handle, request, info);
        return map_server_event(event);
    } catch (...) {
        hako_pdu_rpc_buffer_free(*out_buffer);
        *out_buffer = nullptr;
        *out_size = 0;
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_INTERNAL;
        }
        return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_mux_server_create_reply_buffer(
    hako_pdu_rpc_mux_server_handle_t* handle,
    uint64_t request_token,
    uint8_t status,
    int32_t result_code,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size)
{
    if (handle == nullptr || out_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }

    try {
        RpcMuxRequest request;
        if (!find_request(handle, request_token, request)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        PduData response;
        if (!handle->rpc->create_reply_buffer(
                request,
                static_cast<Hako_uint8>(status),
                static_cast<Hako_int32>(result_code),
                response)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        return copy_pdu(response, buffer, capacity, out_size);
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_mux_server_create_reply_buffer_alloc(
    hako_pdu_rpc_mux_server_handle_t* handle,
    uint64_t request_token,
    uint8_t status,
    int32_t result_code,
    uint8_t** out_buffer,
    size_t* out_size)
{
    reset_output(out_buffer, out_size, nullptr);
    if (handle == nullptr || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }

    try {
        RpcMuxRequest request;
        if (!find_request(handle, request_token, request)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        PduData response;
        if (!handle->rpc->create_reply_buffer(
                request,
                static_cast<Hako_uint8>(status),
                static_cast<Hako_int32>(result_code),
                response)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        return allocate_pdu(response, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_mux_server_send_reply(
    hako_pdu_rpc_mux_server_handle_t* handle,
    uint64_t request_token,
    const uint8_t* pdu,
    size_t pdu_size)
{
    if (handle == nullptr || (pdu_size > 0 && pdu == nullptr)) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }

    try {
        RpcMuxRequest request;
        if (!take_request(handle, request_token, request)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        return handle->rpc->send_reply(request, make_pdu(pdu, pdu_size))
            ? HAKO_PDU_RPC_OK
            : HAKO_PDU_RPC_ERROR_CALL;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_mux_server_send_cancel_reply(
    hako_pdu_rpc_mux_server_handle_t* handle,
    uint64_t request_token,
    const uint8_t* pdu,
    size_t pdu_size)
{
    if (handle == nullptr || (pdu_size > 0 && pdu == nullptr)) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }

    try {
        RpcMuxRequest request;
        if (!take_request(handle, request_token, request)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        return handle->rpc->send_cancel_reply(request, make_pdu(pdu, pdu_size))
            ? HAKO_PDU_RPC_OK
            : HAKO_PDU_RPC_ERROR_CALL;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

size_t hako_pdu_rpc_mux_server_connected_count(
    const hako_pdu_rpc_mux_server_handle_t* handle)
{
    return handle != nullptr && handle->rpc
        ? handle->rpc->connected_count()
        : 0;
}

size_t hako_pdu_rpc_mux_server_expected_count(
    const hako_pdu_rpc_mux_server_handle_t* handle)
{
    return handle != nullptr && handle->rpc
        ? handle->rpc->expected_count()
        : 0;
}

int hako_pdu_rpc_mux_server_is_ready(
    const hako_pdu_rpc_mux_server_handle_t* handle)
{
    return handle != nullptr && handle->rpc && handle->rpc->is_ready() ? 1 : 0;
}

} // extern "C"
