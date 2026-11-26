#include "hakoniwa/pdu/rpc/core_pro_service_api.hpp"

#ifdef HAKO_PDU_RPC_ENABLE_SHM
#include <hakoniwa/hako_asset.h>
#include <cstring>
#include <vector>

namespace hakoniwa::pdu::rpc
{
namespace
{
class RealCoreProServiceApi final : public CoreProServiceApi
{
public:
    bool start_asset(const std::string& asset, const std::string& config, uint64_t delta) override
    {
        hako_asset_callbacks_t callbacks{};
        return hako_asset_register(
                   asset.c_str(), config.c_str(), &callbacks, delta, HAKO_ASSET_MODEL_CONTROLLER) == 0 &&
               hako_asset_start() == 0 && hako_asset_service_initialize(config.c_str()) == 0;
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
        if (!valid(id) ||
                hako_asset_service_client_get_request_buffer(
                    &clients_[id],
                    &packet,
                    &size,
                    HAKO_SERVICE_CLIENT_API_OPCODE_REQUEST,
                    0) != 0 || size < request.size()) return false;
        std::memcpy(packet, request.data(), request.size());
        return hako_asset_service_client_call_request(&clients_[id], packet, request.size(), 0) == 0;
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
    bool step(uint64_t delta) override
    {
        return hako_asset_usleep(delta) == 0;
    }
    void stop_asset() override {}
private:
    bool valid(int id) const
    {
        return id >= 0 && static_cast<size_t>(id) < clients_.size();
    }
    std::vector<HakoServiceHandleType> clients_;
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
