#pragma once

#include "rpc_client_endpoint.hpp"
#include "shm_rpc_asset.hpp"
#include <chrono>
#include <memory>
#include <mutex>

namespace hakoniwa::pdu::rpc
{
class ShmRpcClientEndpoint : public IRpcClientEndpoint,
    public std::enable_shared_from_this<ShmRpcClientEndpoint>
{
public:
    ShmRpcClientEndpoint(
        const std::string& service_name,
        const std::string& client_name,
        uint64_t delta_usec,
        std::shared_ptr<ShmRpcAsset> asset);
    bool initialize(const nlohmann::json& service_config, int pdu_meta_data_size) override;
    bool call(const PduData& pdu, uint64_t timeout_usec) override;
    ClientEventType poll(RpcResponse& response) override;
    bool send_cancel_request() override;
    void create_request_buffer(Hako_uint8 opcode, bool is_cancel_request, PduData& pdu) override;
    void clear_pending_responses() override;
private:
    friend class ShmRpcAsset;

    void asset_step(CoreProServiceApi& api);

    std::shared_ptr<ShmRpcAsset> asset_;
    std::mutex mutex_; // one hand-off box: copied data only while held
    PduData outbox_;
    PduData inbox_;
    bool request_queued_{false};
    bool cancel_queued_{false};
    bool in_flight_{false};
    bool response_ready_{false};
    int core_client_{-1};
    uint64_t next_request_id_{0};
    uint64_t active_request_id_{0};
    uint64_t timeout_usec_{0};
    std::chrono::steady_clock::time_point started_{};
};
} // namespace hakoniwa::pdu::rpc
