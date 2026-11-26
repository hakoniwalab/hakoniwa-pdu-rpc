#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"
#include "nlohmann/json.hpp"
#include "pdu_convertor.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace hakoniwa::pdu::rpc
{
ShmRpcClientEndpoint::ShmRpcClientEndpoint(const std::string& service, const std::string& client, uint64_t delta,
        std::shared_ptr<ShmRpcAsset> asset)
    : IRpcClientEndpoint(service, client, delta), asset_(std::move(asset)) {}

bool ShmRpcClientEndpoint::initialize(const nlohmann::json& config, int metadata_size)
{
    return initialize_without_start(config, metadata_size) && asset_->start();
}

bool ShmRpcClientEndpoint::initialize_without_start(const nlohmann::json& config, int)
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
    if (config.contains("pduSize")) {
        const auto base_size = config.at("pduSize").at("client").at("baseSize").get<std::size_t>();
        response_packet_size_ = HAKO_PDU_META_DATA_SIZE() + HAKO_ALIGN_SIZE(base_size, HAKO_ALIGNMENT_SIZE);
    }
    asset_->add_client(shared_from_this());
    return true;
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
    timeout_reported_ = false;
    started_ = std::chrono::steady_clock::now();
    if (pdu.size() >= 272) {
        HakoCpp_ServiceRequestHeader header{};
        hako::pdu::PduConvertor<
            HakoCpp_ServiceRequestHeader,
            hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader> {}.pdu2cpp(
            reinterpret_cast<char*>(outbox_.data()), header);
        active_request_id_ = header.request_id;
    }
    return true;
}
bool ShmRpcClientEndpoint::send_cancel_request()
{
    std::lock_guard lock(mutex_);
    if (!in_flight_ || response_ready_ || cancel_state_ != CancelState::none) return false;
    cancel_state_ = CancelState::pending;
    return true;
}
void ShmRpcClientEndpoint::create_request_buffer(Hako_uint8 opcode, bool is_cancel, PduData& pdu)
{
    // Generated core-pro PDU converters need room for their wire metadata and
    // fixed string fields; sizeof the C++ header (80 bytes) is not a wire size.
    pdu.resize(512);
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
    if (response_ready_) {
        response.pdu = std::move(inbox_);
        response_ready_ = false;
        in_flight_ = false;
        timeout_reported_ = false;
        hako::pdu::PduConvertor<
            HakoCpp_ServiceResponseHeader,
            hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> {}.pdu2cpp(
            reinterpret_cast<char*>(response.pdu.data()), response.header);
        return response.header.result_code == HAKO_SERVICE_RESULT_CODE_CANCELED
            ? ClientEventType::RESPONSE_CANCEL
            : ClientEventType::RESPONSE_IN;
    }
    if (in_flight_ && timeout_usec_ && !timeout_reported_ &&
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_).count() >
            timeout_usec_) {
        timeout_reported_ = true;
        return ClientEventType::RESPONSE_TIMEOUT;
    }
    return ClientEventType::NONE;
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
        if (request_queued_) {
            request = outbox_;
            request_queued_ = false;
            send = true;
        }
        cancel = cancel_state_ == CancelState::pending;
    }
    if (send && !api.call(core_client_, request)) {
        asset_fail_request();
        return;
    }
    const auto event = api.poll(core_client_);
    if (event != CoreProClientEvent::response_in && event != CoreProClientEvent::cancel_done) {
        // The cancel shares the request channel. Retry on the next step if its PDU is still unread.
        if (cancel && api.cancel(core_client_)) {
            std::lock_guard lock(mutex_);
            cancel_state_ = CancelState::sent;
        }
        return;
    }
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
        HakoPduMetaDataType metadata{};
        std::memcpy(&metadata, reply.data(), sizeof(metadata));
        header.request_id = active_request_id_;
        hako::pdu::PduConvertor<
            HakoCpp_ServiceResponseHeader,
            hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> {}.cpp2pdu(
            header, reinterpret_cast<char*>(reply.data()), reply.size());
        // The generated header writer replaces the full packet's metadata.
        std::memcpy(reply.data(), &metadata, sizeof(metadata));
        inbox_ = std::move(reply);
        response_ready_ = true;
        cancel_state_ = CancelState::none;
    }
}
bool ShmRpcClientEndpoint::asset_started(CoreProServiceApi& api)
{
    std::lock_guard lock(mutex_);
    core_client_ = api.create_client(asset_->asset_name(), service_name_, client_name_);
    return core_client_ >= 0;
}
void ShmRpcClientEndpoint::asset_fail_request()
{
    std::lock_guard lock(mutex_);
    request_queued_ = false;
    cancel_state_ = CancelState::none;
    if (!in_flight_) return;
    HakoCpp_ServiceResponseHeader header{};
    header.request_id = active_request_id_;
    header.service_name = service_name_;
    header.client_name = client_name_;
    header.status = HAKO_SERVICE_STATUS_DONE;
    header.result_code = HAKO_SERVICE_RESULT_CODE_ERROR;
    inbox_.assign(std::max<std::size_t>(512, response_packet_size_), 0);
    const int size = hako::pdu::PduConvertor<
        HakoCpp_ServiceResponseHeader,
        hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader> {}.cpp2pdu(
        header, reinterpret_cast<char*>(inbox_.data()), inbox_.size());
    if (size < 0) {
        inbox_.clear();
        return;
    }
    const auto packet_size = std::max(static_cast<std::size_t>(size), response_packet_size_);
    inbox_.resize(packet_size);
    // A short header cannot be decoded by a typed client. Include an empty
    // response body and describe its extent in the packet metadata.
    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, inbox_.data(), sizeof(metadata));
    metadata.heap_off = packet_size;
    metadata.total_size = packet_size;
    std::memcpy(inbox_.data(), &metadata, sizeof(metadata));
    response_ready_ = true;
}
} // namespace hakoniwa::pdu::rpc
