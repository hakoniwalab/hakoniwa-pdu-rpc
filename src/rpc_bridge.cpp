#include "rpc_bridge.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace hakoniwa::pdu::rpc {
namespace {

using RequestHeader = hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader;
using ResponseHeader = hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader;

template<class WireHeader, class Converter, class Header>
bool replace_header(PduData& pdu, Header& header)
{
    constexpr size_t base_off = HAKO_PDU_META_DATA_SIZE();
    constexpr size_t aligned_header_size = HAKO_ALIGN_SIZE(sizeof(WireHeader), HAKO_ALIGNMENT_SIZE);
    std::array<char, base_off + aligned_header_size> encoded_header;
    if (pdu.size() < encoded_header.size()
        || pdu.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, pdu.data(), sizeof(metadata));
    if (metadata.base_off != base_off
        || Converter{}.cpp2pdu(header, encoded_header.data(), encoded_header.size()) < 0) return false;
    // The standalone encoder writes metadata and trailing padding. Copy only the embedded header.
    std::memcpy(pdu.data() + base_off, encoded_header.data() + base_off, sizeof(WireHeader));
    return true;
}

} // namespace

RpcBridge::RpcBridge(IRpcServerEndpoint& server, IRpcClientEndpoint& client)
    : server_(server), client_(client)
{
}

void RpcBridge::poll()
{
    receive_request();
    if (outgoing_id_) receive_response();
    if (!outgoing_id_ && !pending_.empty()) forward_request();
    if (outgoing_id_) {
        auto& active = pending_.front();
        if (active.cancel_requested && !active.cancel_sent) {
            active.cancel_sent = client_.send_cancel_request();
        }
    }
}

void RpcBridge::receive_request()
{
    RpcRequest request{};
    const auto event = server_.poll(request);
    if (event == ServerEventType::NONE) return;

    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const auto& pending) {
        return pending.request.header.client_name == request.header.client_name;
    });
    if (event == ServerEventType::REQUEST_CANCEL) {
        if (found != pending_.end() && found->request.header.request_id == request.header.request_id) {
            found->cancel_requested = true;
        }
    } else if (found != pending_.end()) {
        reply_with_error(request.header, HAKO_SERVICE_RESULT_CODE_BUSY);
    } else {
        pending_.push_back({std::move(request)});
    }
}

void RpcBridge::forward_request()
{
    auto& request = pending_.front().request;
    PduData header_buffer;
    HakoCpp_ServiceRequestHeader outgoing{};
    client_.create_request_buffer(HAKO_SERVICE_OPERATION_CODE_REQUEST, false, header_buffer);
    if (!header_buffer.empty()
        && RequestHeader{}.pdu2cpp(reinterpret_cast<char*>(header_buffer.data()), outgoing)
        && replace_header<Hako_ServiceRequestHeader, RequestHeader>(request.pdu, outgoing)
        && client_.call(request.pdu, 0)) {
        outgoing_id_ = outgoing.request_id;
        request.pdu.clear();
        return;
    }
    reply_with_error(request.header, HAKO_SERVICE_RESULT_CODE_ERROR);
    pending_.pop_front();
}

void RpcBridge::receive_response()
{
    RpcResponse response{};
    const auto event = client_.poll(response);
    if (event != ClientEventType::RESPONSE_IN && event != ClientEventType::RESPONSE_CANCEL) return;
    auto& header = response.header;
    if (header.request_id != *outgoing_id_ || header.client_name != client_.get_client_name()
        || header.service_name != client_.get_service_name() || is_interim_response(header.status, header.result_code)) return;

    const auto& active = pending_.front();
    const auto& original = active.request.header;
    header.request_id = original.request_id;
    header.client_name = original.client_name;
    header.service_name = original.service_name;
    if (!replace_header<Hako_ServiceResponseHeader, ResponseHeader>(response.pdu, header)) {
        reply_with_error(original, HAKO_SERVICE_RESULT_CODE_ERROR);
    } else if (active.cancel_requested && header.result_code == HAKO_SERVICE_RESULT_CODE_CANCELED) {
        server_.send_cancel_reply(original.client_name, response.pdu);
    } else {
        server_.send_reply(original.client_name, response.pdu);
    }
    pending_.pop_front();
    outgoing_id_.reset();
}

void RpcBridge::reply_with_error(const HakoCpp_ServiceRequestHeader& request, Hako_int32 result)
{
    PduData reply;
    const auto status = result == HAKO_SERVICE_RESULT_CODE_BUSY
        ? HAKO_SERVICE_STATUS_DOING : HAKO_SERVICE_STATUS_ERROR;
    server_.create_reply_buffer(request, status, result, reply);
    server_.send_reply(request.client_name, reply);
}

} // namespace hakoniwa::pdu::rpc
