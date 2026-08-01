#include "hakoniwa/pdu/rpc/c_rpc.h"

#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"

#include <map>
#include <memory>
#include <mutex>

using hakoniwa::pdu::EndpointContainer;
using hakoniwa::pdu::rpc::PduData;
using hakoniwa::pdu::rpc::RpcRequest;
using hakoniwa::pdu::rpc::RpcServicesClient;
using hakoniwa::pdu::rpc::RpcServicesServer;

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

PduData make_pdu(const uint8_t* data, size_t size)
{
    return size == 0 ? PduData{} : PduData(data, data + size);
}
} // namespace

extern "C" {

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
    try {
        return handle->rpc->send_cancel_request(service_name)
            ? HAKO_PDU_RPC_OK
            : HAKO_PDU_RPC_ERROR_CALL;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

hako_pdu_rpc_error_t hako_pdu_rpc_server_send_cancel_reply(
    hako_pdu_rpc_server_handle_t* handle,
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

    try {
        handle->rpc->send_cancel_reply(
            request.header,
            make_pdu(pdu, pdu_size));
        return HAKO_PDU_RPC_OK;
    } catch (...) {
        return HAKO_PDU_RPC_ERROR_INTERNAL;
    }
}

} // extern "C"
