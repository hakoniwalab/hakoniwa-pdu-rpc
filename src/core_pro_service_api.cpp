#include "hakoniwa/pdu/rpc/core_pro_service_api.hpp"

#ifdef HAKO_PDU_RPC_ENABLE_SHM
#include <hakoniwa_asset_polling.h>
#include <hako_srv_msgs/pdu_ctype_ServiceRequestHeader.h>
#include <cstring>
#include <vector>

namespace hakoniwa::pdu::rpc
{
namespace
{
class RealCoreProServiceApi final : public CoreProServiceApi
{
public:
    bool initialize_asset(
        const std::string& asset,
        const std::string& core_config,
        const std::string& service_config) override
    {
        if (!core_config.empty()) setenv("HAKO_CONFIG_PATH", core_config.c_str(), 1);
        return hakoniwa_asset_init() == 0 && hakoniwa_simevent_init() == 0 &&
               hakoniwa_asset_register_polling(asset.c_str()) == 0 &&
               hako_asset_service_initialize(service_config.c_str()) == 0;
    }
    CoreProAssetEvent asset_event(const std::string& asset) override
    {
        switch (hakoniwa_asset_get_event(asset.c_str())) {
        case 1:
            local_time_ = 0;
            return CoreProAssetEvent::start;
        case 2: return CoreProAssetEvent::stop;
        case 3:
            local_time_ = 0;
            return CoreProAssetEvent::reset;
        case 4: return CoreProAssetEvent::error;
        default: return CoreProAssetEvent::none;
        }
    }
    bool pdu_created() override
    {
        // Besides testing readiness, core-pro loads this process's PDU data and
        // service table here. It must precede service client creation on Start.
        return hakoniwa_asset_is_pdu_created();
    }
    bool start_feedback(const std::string& asset) override
    {
        return hakoniwa_asset_notify_write_pdu_done(asset.c_str()),
               hakoniwa_asset_start_feedback_ok(asset.c_str()) == 0;
    }
    bool stop_feedback(const std::string& asset) override
    {
        return hakoniwa_asset_stop_feedback_ok(asset.c_str()) == 0;
    }
    bool reset_feedback(const std::string& asset) override
    {
        return hakoniwa_asset_reset_feedback_ok(asset.c_str()) == 0;
    }
    bool running() override
    {
        return hakoniwa_simevent_get_state() == 2 && hakoniwa_asset_is_pdu_created();
    }
    void advance_time(const std::string& asset, uint64_t delta) override
    {
        if (local_time_ <= hakoniwa_asset_get_worldtime()) {
            local_time_ += delta;
            hakoniwa_asset_notify_simtime(asset.c_str(), local_time_);
        }
    }
    int create_client(const std::string& asset, const std::string& service, const std::string& client) override
    {
        HakoServiceHandleType handle{};
        if (hako_asset_service_client_create(asset.c_str(), service.c_str(), client.c_str(), &handle) != 0) return -1;
        clients_.push_back(handle);
        return static_cast<int>(clients_.size() - 1);
    }
    bool call(int id, const std::vector<uint8_t>& request) override
    {
        char* packet{};
        size_t size{};
        const size_t request_size = request.size();
        if (!valid(id) || request_size == 0 ||
                hako_asset_service_client_get_request_buffer(
                    &clients_[id],
                    &packet,
                    &size,
                    HAKO_SERVICE_CLIENT_API_OPCODE_REQUEST,
                    0) != 0 || size < request_size) return false;
        constexpr size_t base_off = HAKO_PDU_META_DATA_SIZE();
        constexpr size_t body_offset = base_off + sizeof(Hako_ServiceRequestHeader);
        if (request_size < body_offset || size < body_offset) return false;
        // Keep Core's header. The full packet's body can start before the
        // padding at the end of a standalone header PDU.
        std::memcpy(packet, request.data(), base_off);
        std::memcpy(packet + body_offset, request.data() + body_offset, request_size - body_offset);
        return hako_asset_service_client_call_request(&clients_[id], packet, size, 0) == 0;
    }
    bool cancel(int id) override
    {
        return valid(id) && hako_asset_service_client_cancel_request(&clients_[id]) == 0;
    }
    CoreProClientEvent poll(int id) override
    {
        if (!valid(id)) return CoreProClientEvent::none;
        switch (hako_asset_service_client_poll(&clients_[id])) {
            case HAKO_SERVICE_CLIENT_API_RESPONSE_IN:
                return CoreProClientEvent::response_in;
            case HAKO_SERVICE_CLIENT_API_REQUEST_CANCEL_DONE:
                return CoreProClientEvent::cancel_done;
            case HAKO_SERVICE_CLIENT_API_REQUEST_TIMEOUT:
                return CoreProClientEvent::timeout;
            default:
                return CoreProClientEvent::none;
        }
    }
    bool response(int id, std::vector<uint8_t>& result) override
    {
        char* packet{};
        size_t size{};
        if (!valid(id) || hako_asset_service_client_get_response(&clients_[id], &packet, &size, 0) != 0) return false;
        result.assign(packet, packet + size);
        return true;
    }
    void stop_asset() override {}
private:
    bool valid(int id) const
    {
        return id >= 0 && static_cast<size_t>(id) < clients_.size();
    }
    std::vector<HakoServiceHandleType> clients_;
    hako_time_t local_time_{0};
};
}
std::shared_ptr<CoreProServiceApi> make_core_pro_service_api()
{
    return std::make_shared<RealCoreProServiceApi>();
}
} // namespace hakoniwa::pdu::rpc
#else
namespace hakoniwa::pdu::rpc
{
std::shared_ptr<CoreProServiceApi> make_core_pro_service_api()
{
    return {};
}
}
#endif
