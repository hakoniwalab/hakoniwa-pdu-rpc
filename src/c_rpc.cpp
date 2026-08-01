#include "hakoniwa/pdu/rpc/c_rpc.h"

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using hakoniwa::pdu::EndpointContainer;
using hakoniwa::pdu::rpc::ClientEventType;
using hakoniwa::pdu::rpc::PduData;
using hakoniwa::pdu::rpc::RpcRequest;
using hakoniwa::pdu::rpc::RpcResponse;
using hakoniwa::pdu::rpc::RpcServicesClient;
using hakoniwa::pdu::rpc::RpcServicesServer;
using hakoniwa::pdu::rpc::ServerEventType;

namespace {
constexpr const char* kClientImpl = "RpcClientEndpointImpl";
constexpr const char* kServerImpl = "RpcServerEndpointImpl";
constexpr const char* kDefaultTimeSource = "real";

bool valid_text(const char* value) { return value != nullptr && value[0] != '\0'; }

void copy_name(char* dst, size_t capacity, const std::string& src)
{
    if (dst == nullptr || capacity == 0) return;
    const size_t count = std::min(capacity - 1, src.size());
    std::memcpy(dst, src.data(), count);
    dst[count] = '\0';
}

hako_pdu_rpc_error_t copy_pdu(const PduData& src, uint8_t* dst, size_t capacity, size_t* out_size)
{
    if (out_size == nullptr) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    *out_size = src.size();
    if (src.size() > capacity || (!src.empty() && dst == nullptr)) return HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL;
    if (!src.empty()) std::memcpy(dst, src.data(), src.size());
    return HAKO_PDU_RPC_OK;
}

PduData make_pdu(const uint8_t* data, size_t size)
{
    return size == 0 ? PduData{} : PduData(data, data + size);
}

hako_pdu_rpc_client_event_t map_client_event(ClientEventType event)
{
    switch (event) {
    case ClientEventType::RESPONSE_IN: return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN;
    case ClientEventType::RESPONSE_CANCEL: return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL;
    case ClientEventType::RESPONSE_TIMEOUT: return HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT;
    default: return HAKO_PDU_RPC_CLIENT_EVENT_NONE;
    }
}

hako_pdu_rpc_server_event_t map_server_event(ServerEventType event)
{
    switch (event) {
    case ServerEventType::REQUEST_IN: return HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN;
    case ServerEventType::REQUEST_CANCEL: return HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL;
    default: return HAKO_PDU_RPC_SERVER_EVENT_NONE;
    }
}
} // namespace

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
void stop_client_endpoint(hako_pdu_rpc_client_handle_t* h) { if (h && h->endpoints) (void)h->endpoints->stop_all(); }
void stop_server_endpoint(hako_pdu_rpc_server_handle_t* h) { if (h && h->endpoints) (void)h->endpoints->stop_all(); }
void stop_client_rpc(hako_pdu_rpc_client_handle_t* h) { if (h && h->rpc) { h->rpc->stop_all_services(); h->rpc->clear_all_instances(); } }
void stop_server_rpc(hako_pdu_rpc_server_handle_t* h) { if (h && h->rpc) { h->rpc->stop_all_services(); h->rpc->clear_all_instances(); } }
void release_client(hako_pdu_rpc_client_handle_t* h) { if (h) { h->rpc.reset(); h->endpoints.reset(); h->started = false; } }
void release_server(hako_pdu_rpc_server_handle_t* h) {
    if (!h) return;
    { std::lock_guard<std::mutex> lock(h->pending_mutex); h->pending_requests.clear(); }
    h->rpc.reset(); h->endpoints.reset(); h->started = false;
}
} // namespace

extern "C" {

hako_pdu_rpc_client_handle_t* hako_pdu_rpc_client_create(const char* node_id, const char* client_name, const char* service_config_path, const char* endpoint_config_path, uint64_t delta_time_usec, const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(client_name) || !valid_text(service_config_path) || !valid_text(endpoint_config_path)) return nullptr;
    try {
        auto h = std::make_unique<hako_pdu_rpc_client_handle_t>();
        h->endpoints = std::make_shared<EndpointContainer>(node_id, endpoint_config_path);
        if (h->endpoints->initialize() != HAKO_PDU_ERR_OK) return nullptr;
        h->rpc = std::make_unique<RpcServicesClient>(node_id, client_name, service_config_path, kClientImpl, delta_time_usec, valid_text(time_source_type) ? time_source_type : kDefaultTimeSource);
        if (!h->rpc->initialize_services(h->endpoints)) return nullptr;
        return h.release();
    } catch (...) { return nullptr; }
}

void hako_pdu_rpc_client_destroy(hako_pdu_rpc_client_handle_t* h)
{
    if (!h) return;
    (void)hako_pdu_rpc_client_stop(h);
    delete h;
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_start(hako_pdu_rpc_client_handle_t* h)
{
    if (!h) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    if (h->started) return HAKO_PDU_RPC_OK;
    if (!h->endpoints || !h->rpc) return HAKO_PDU_RPC_ERROR_INITIALIZE;
    try {
        if (h->endpoints->start_all() != HAKO_PDU_ERR_OK) return HAKO_PDU_RPC_ERROR_START;
        if (!h->rpc->start_all_services()) { stop_client_endpoint(h); return HAKO_PDU_RPC_ERROR_START; }
        h->started = true;
        return HAKO_PDU_RPC_OK;
    } catch (...) { stop_client_endpoint(h); return HAKO_PDU_RPC_ERROR_INTERNAL; }
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_stop(hako_pdu_rpc_client_handle_t* h)
{
    if (!h) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    stop_client_endpoint(h);
    stop_client_rpc(h);
    release_client(h);
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_create_request_buffer(hako_pdu_rpc_client_handle_t* h, const char* service_name, uint8_t* buffer, size_t capacity, size_t* out_size)
{
    if (!h || !valid_text(service_name) || !out_size) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    if (!h->started || !h->rpc) return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    PduData pdu;
    if (!h->rpc->create_request_buffer(service_name, pdu)) return HAKO_PDU_RPC_ERROR_NOT_FOUND;
    return copy_pdu(pdu, buffer, capacity, out_size);
}

hako_pdu_rpc_error_t hako_pdu_rpc_client_call(hako_pdu_rpc_client_handle_t* h, const char* service_name, const uint8_t* pdu, size_t pdu_size, uint64_t timeout_usec)
{
    if (!h || !valid_text(service_name) || (pdu_size > 0 && !pdu)) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    if (!h->started || !h->rpc) return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    return h->rpc->call(service_name, make_pdu(pdu, pdu_size), timeout_usec) ? HAKO_PDU_RPC_OK : HAKO_PDU_RPC_ERROR_CALL;
}

hako_pdu_rpc_client_event_t hako_pdu_rpc_client_poll(hako_pdu_rpc_client_handle_t* h, hako_pdu_rpc_response_info_t* info, uint8_t* buffer, size_t capacity, size_t* out_size, hako_pdu_rpc_error_t* out_error)
{
    if (out_error) *out_error = HAKO_PDU_RPC_OK;
    if (!h || !info || !out_size) { if (out_error) *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT; return HAKO_PDU_RPC_CLIENT_EVENT_NONE; }
    if (!h->started || !h->rpc) { if (out_error) *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING; return HAKO_PDU_RPC_CLIENT_EVENT_NONE; }
    std::string service_name;
    RpcResponse response;
    const auto event = h->rpc->poll(service_name, response);
    if (event == ClientEventType::NONE) { *out_size = 0; return HAKO_PDU_RPC_CLIENT_EVENT_NONE; }
    const auto result = copy_pdu(response.pdu, buffer, capacity, out_size);
    if (result != HAKO_PDU_RPC_OK) { if (out_error) *out_error = result; return HAKO_PDU_RPC_CLIENT_EVENT_NONE; }
    copy_name(info->service_name, sizeof(info->service_name), service_name);
    info->pdu_size = response.pdu.size();
    return map_client_event(event);
}

hako_pdu_rpc_server_handle_t* hako_pdu_rpc_server_create(const char* node_id, const char* service_config_path, const char* endpoint_config_path, uint64_t delta_time_usec, const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(service_config_path) || !valid_text(endpoint_config_path)) return nullptr;
    try {
        auto h = std::make_unique<hako_pdu_rpc_server_handle_t>();
        h->endpoints = std::make_shared<EndpointContainer>(node_id, endpoint_config_path);
        if (h->endpoints->initialize() != HAKO_PDU_ERR_OK) return nullptr;
        h->rpc = std::make_unique<RpcServicesServer>(node_id, kServerImpl, service_config_path, delta_time_usec, valid_text(time_source_type) ? time_source_type : kDefaultTimeSource);
        if (!h->rpc->initialize_services(h->endpoints)) return nullptr;
        return h.release();
    } catch (...) { return nullptr; }
}

void hako_pdu_rpc_server_destroy(hako_pdu_rpc_server_handle_t* h)
{
    if (!h) return;
    (void)hako_pdu_rpc_server_stop(h);
    delete h;
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_start(hako_pdu_rpc_server_handle_t* h)
{
    if (!h) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    if (h->started) return HAKO_PDU_RPC_OK;
    if (!h->endpoints || !h->rpc) return HAKO_PDU_RPC_ERROR_INITIALIZE;
    try {
        if (h->endpoints->start_all() != HAKO_PDU_ERR_OK) return HAKO_PDU_RPC_ERROR_START;
        if (!h->rpc->start_all_services()) { stop_server_endpoint(h); return HAKO_PDU_RPC_ERROR_START; }
        h->started = true;
        return HAKO_PDU_RPC_OK;
    } catch (...) { stop_server_endpoint(h); return HAKO_PDU_RPC_ERROR_INTERNAL; }
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_stop(hako_pdu_rpc_server_handle_t* h)
{
    if (!h) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    stop_server_endpoint(h);
    stop_server_rpc(h);
    release_server(h);
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_server_event_t hako_pdu_rpc_server_poll(hako_pdu_rpc_server_handle_t* h, hako_pdu_rpc_request_info_t* info, uint8_t* buffer, size_t capacity, size_t* out_size, hako_pdu_rpc_error_t* out_error)
{
    if (out_error) *out_error = HAKO_PDU_RPC_OK;
    if (!h || !info || !out_size) { if (out_error) *out_error = HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT; return HAKO_PDU_RPC_SERVER_EVENT_NONE; }
    if (!h->started || !h->rpc) { if (out_error) *out_error = HAKO_PDU_RPC_ERROR_NOT_RUNNING; return HAKO_PDU_RPC_SERVER_EVENT_NONE; }
    RpcRequest request;
    const auto event = h->rpc->poll(request);
    if (event == ServerEventType::NONE) { *out_size = 0; return HAKO_PDU_RPC_SERVER_EVENT_NONE; }
    const auto result = copy_pdu(request.pdu, buffer, capacity, out_size);
    if (result != HAKO_PDU_RPC_OK) { if (out_error) *out_error = result; return HAKO_PDU_RPC_SERVER_EVENT_NONE; }
    uint64_t token;
    { std::lock_guard<std::mutex> lock(h->pending_mutex); token = h->next_token++; h->pending_requests.emplace(token, request); }
    info->request_token = token;
    copy_name(info->service_name, sizeof(info->service_name), request.header.service_name);
    copy_name(info->client_name, sizeof(info->client_name), request.client_name);
    info->pdu_size = request.pdu.size();
    return map_server_event(event);
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_create_reply_buffer(hako_pdu_rpc_server_handle_t* h, uint64_t token, uint8_t status, int32_t result_code, uint8_t* buffer, size_t capacity, size_t* out_size)
{
    if (!h || !out_size) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    if (!h->started || !h->rpc) return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    RpcRequest request;
    { std::lock_guard<std::mutex> lock(h->pending_mutex); const auto it = h->pending_requests.find(token); if (it == h->pending_requests.end()) return HAKO_PDU_RPC_ERROR_NOT_FOUND; request = it->second; }
    PduData response;
    h->rpc->create_reply_buffer(request.header, static_cast<Hako_uint8>(status), static_cast<Hako_int32>(result_code), response);
    return copy_pdu(response, buffer, capacity, out_size);
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_send_reply(hako_pdu_rpc_server_handle_t* h, uint64_t token, const uint8_t* pdu, size_t pdu_size)
{
    if (!h || (pdu_size > 0 && !pdu)) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    if (!h->started || !h->rpc) return HAKO_PDU_RPC_ERROR_NOT_RUNNING;
    RpcRequest request;
    { std::lock_guard<std::mutex> lock(h->pending_mutex); const auto it = h->pending_requests.find(token); if (it == h->pending_requests.end()) return HAKO_PDU_RPC_ERROR_NOT_FOUND; request = it->second; h->pending_requests.erase(it); }
    h->rpc->send_reply(request.header, make_pdu(pdu, pdu_size));
    return HAKO_PDU_RPC_OK;
}

hako_pdu_rpc_error_t hako_pdu_rpc_stop_pair(hako_pdu_rpc_server_handle_t* server, hako_pdu_rpc_client_handle_t* client)
{
    if (!server || !client) return HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT;
    stop_server_endpoint(server);
    stop_client_endpoint(client);
    stop_server_rpc(server);
    stop_client_rpc(client);
    release_server(server);
    release_client(client);
    return HAKO_PDU_RPC_OK;
}

} // extern "C"
