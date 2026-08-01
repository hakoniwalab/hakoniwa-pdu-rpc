#include "hakoniwa/pdu/rpc/c_rpc.h"

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using hakoniwa::pdu::EndpointContainer;
using hakoniwa::pdu::rpc::ClientEventType;
using hakoniwa::pdu::rpc::PduData;
using hakoniwa::pdu::rpc::RpcRequest;
using hakoniwa::pdu::rpc::RpcResponse;
using hakoniwa::pdu::rpc::RpcServicesClient;
using hakoniwa::pdu::rpc::RpcServicesServer;
using hakoniwa::pdu::rpc::ServerEventType;

/* Keep these definitions token-compatible with c_rpc.cpp. */
struct hako_pdu_rpc_client_handle {
    std::shared_ptr<EndpointContainer> endpoints;
    std::unique_ptr<RpcServicesClient> rpc;
    bool started{false};
};

struct hako_pdu_rpc_server_handle {
    std::shared_ptr<EndpointContainer> endpoints;
    std::unique_ptr<RpcServicesServer> rpc;
    std::mutex pending_mutex;
    uint64_t next_token{1};
    std::map<uint64_t, RpcRequest> pending_requests;
    bool started{false};
};

namespace {

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

hako_pdu_rpc_client_event_t map_client_event(ClientEventType event)
{
    switch (event) {
    case ClientEventType::RESPONSE_IN:
        return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN;
    case ClientEventType::RESPONSE_CANCEL:
        return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL;
    case ClientEventType::RESPONSE_TIMEOUT:
        return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT;
    default:
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
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

extern "C" {

void hako_pdu_rpc_buffer_free(uint8_t* buffer)
{
    std::free(buffer);
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_create_request_buffer_alloc(
    hako_pdu_rpc_client_handle_t* handle,
    const char* service_name,
    uint8_t** out_buffer,
    size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || !valid_text(service_name) || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }

    try {
        PduData pdu;
        if (!handle->rpc->create_request_buffer(service_name, pdu)) {
            return HAKO_PDU_RPC_ERROR_NOT_FOUND;
        }
        return allocate_pdu(pdu, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_client_event_t hako_pdu_rpc_client_poll_alloc(
    hako_pdu_rpc_client_handle_t* handle,
    hako_pdu_rpc_response_info_t* info,
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
    if (handle == nullptr || info == nullptr || out_buffer == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
    if (!handle->started || !handle->rpc) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }

    try {
        std::string service_name;
        RpcResponse response;
        const auto event = handle->rpc->poll(service_name, response);
        if (event == ClientEventType::NONE) {
            return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
        }

        const auto result = allocate_pdu(response.pdu, out_buffer, out_size);
        if (result != HAKO_PDU_RPC_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
        }
        copy_name(info->service_name, sizeof(info->service_name), service_name);
        info->pdu_size = response.pdu.size();
        return map_client_event(event);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_RPC_ERROR_INTERNAL;
        }
        return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
}

hako_pdu_rpc_server_event_t hako_pdu_rpc_server_poll_alloc(
    hako_pdu_rpc_server_handle_t* handle,
    hako_pdu_rpc_request_info_t* info,
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
    if (handle == nullptr || info == nullptr || out_buffer == nullptr || out_size == nullptr) {
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
        RpcRequest request;
        const auto event = handle->rpc->poll(request);
        if (event == ServerEventType::NONE) {
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }

        const auto result = allocate_pdu(request.pdu, out_buffer, out_size);
        if (result != HAKO_PDU_RPC_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_RPC_SERVER_EVENT_NONE;
        }

        uint64_t token = 0;
        {
            std::lock_guard<std::mutex> lock(handle->pending_mutex);
            token = handle->next_token++;
            handle->pending_requests.emplace(token, request);
        }
        info->request_token = token;
        copy_name(info->service_name, sizeof(info->service_name), request.header.service_name);
        copy_name(info->client_name, sizeof(info->client_name), request.client_name);
        info->pdu_size = request.pdu.size();
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

hako_pdu_rpc_error_t hako_pdu_rpc_server_create_reply_buffer_alloc(
    hako_pdu_rpc_server_handle_t* handle,
    uint64_t request_token,
    uint8_t status,
    int32_t result_code,
    uint8_t** out_buffer,
    size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->rpc) {
        return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    }

    try {
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
        return allocate_pdu(response, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

} // extern "C"
