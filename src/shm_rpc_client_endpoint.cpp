#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"
#include "nlohmann/json.hpp"
#include "pdu_convertor.hpp"
#include <cstring>
#include <iostream>

namespace hakoniwa::pdu::rpc
{
ShmRpcClientEndpoint::ShmRpcClientEndpoint(const std::string& service, const std::string& client, uint64_t delta,
        std::shared_ptr<ShmRpcAsset> asset)
    : IRpcClientEndpoint(service, client, delta), asset_(std::move(asset)) {}

bool ShmRpcClientEndpoint::initialize(const nlohmann::json& config, int)
{
#ifndef HAKO_PDU_RPC_ENABLE_SHM
    (void)config;
    std::cerr << "ERROR: protocol 'shm' is unavailable: build with HAKO_PDU_RPC_ENABLE_SHM=ON" << std::endl;
    return false;
#else
    if (!asset_ || !asset_->enabled()) {
        std::cerr << "ERROR: protocol 'shm' is unavailable: build with HAKO_PDU_RPC_ENABLE_SHM=ON" << std::endl;
        return false;
    }
    if (!config.contains("name") || config["name"] != service_name_) return false;
    asset_->add_client(shared_from_this());
    return asset_->start();
#endif
}
bool ShmRpcClientEndpoint::call(const PduData& pdu, uint64_t timeout)
{
    std::lock_guard lock(mutex_);
    if (in_flight_ || request_queued_) {
        std::cerr << "ERROR: SHM RPC client is busy" << std::endl;
        return false;
    }
    outbox_ = pdu;
    request_queued_ = true;
    in_flight_ = true;
    timeout_usec_ = timeout;
    started_ = std::chrono::steady_clock::now();
    active_request_id_ = next_request_id_;
    return true;
}
bool ShmRpcClientEndpoint::send_cancel_request()
{
    std::lock_guard lock(mutex_);
    if (!in_flight_ || cancel_queued_) return false;
    cancel_queued_ = true;
    return true;
}
void ShmRpcClientEndpoint::create_request_buffer(Hako_uint8 opcode, bool is_cancel, PduData& pdu)
{
    pdu.resize(sizeof(HakoCpp_ServiceRequestHeader));
    HakoCpp_ServiceRequestHeader header{};
    header.request_id = is_cancel ? active_request_id_ : ++next_request_id_;
    header.client_name = client_name_;
    header.service_name = service_name_;
    header.opcode = opcode;
    hako::pdu::PduConvertor<
        HakoCpp_ServiceRequestHeader,
        hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader> {}.cpp2pdu(
        header, reinterpret_cast<char*>(pdu.data()), pdu.size());
}
ClientEventType ShmRpcClientEndpoint::poll(RpcResponse& response)
{
    std::lock_guard lock(mutex_);
    if (in_flight_ && timeout_usec_ &&
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_).count() >
            timeout_usec_) return ClientEventType::RESPONSE_TIMEOUT;
    if (!response_ready_) return ClientEventType::NONE;
    response.pdu = std::move(inbox_);
    response_ready_ = false;
    in_flight_ = false;
    cancel_queued_ = false;
    hako::pdu::PduConvertor<
        HakoCpp_ServiceResponseHeader,
        hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> {}.pdu2cpp(
        reinterpret_cast<char*>(response.pdu.data()), response.header);
    return response.header.result_code == HAKO_SERVICE_RESULT_CODE_CANCELED ? ClientEventType::RESPONSE_CANCEL :
           ClientEventType::RESPONSE_IN;
}
void ShmRpcClientEndpoint::clear_pending_responses()
{
    std::lock_guard lock(mutex_);
    inbox_.clear();
    response_ready_ = false;
}
void ShmRpcClientEndpoint::asset_step(CoreProServiceApi& api)
{
    PduData request;
    bool send = false, cancel = false;
    {
        std::lock_guard lock(mutex_);
        if (core_client_ < 0) core_client_ = api.create_client(asset_->asset_name(), service_name_, client_name_);
        if (request_queued_) {
            request = outbox_;
            request_queued_ = false;
            send = true;
        }
        if (cancel_queued_) {
            cancel_queued_ = false;
            cancel = true;
        }
    }
    if (core_client_ < 0) return;
    if (send) api.call(core_client_, request);
    if (cancel) api.cancel(core_client_);
    const auto event = api.poll(core_client_);
    if (event != CoreProClientEvent::response_in && event != CoreProClientEvent::cancel_done) return;
    PduData reply;
    if (!api.response(core_client_, reply)) return;
    HakoCpp_ServiceResponseHeader header{};
    hako::pdu::PduConvertor<
        HakoCpp_ServiceResponseHeader,
        hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> {}.pdu2cpp(
        reinterpret_cast<char*>(reply.data()), header);
    // core-pro #123: BUSY and progress/DOING are notices, not a terminal reply.
    if (header.result_code == HAKO_SERVICE_RESULT_CODE_BUSY || header.status == HAKO_SERVICE_STATUS_DOING) return;
    std::lock_guard lock(mutex_);
    if (in_flight_) {
        inbox_ = std::move(reply);
        response_ready_ = true;
    }
}
} // namespace hakoniwa::pdu::rpc
