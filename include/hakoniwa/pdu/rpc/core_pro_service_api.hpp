#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hakoniwa::pdu::rpc
{

enum class CoreProClientEvent {
    none,
    response_in,
    cancel_done,
    timeout,
};

// This deliberately small facade is the only seam used by unit tests.  Production
// uses the C core-pro service API through make_core_pro_service_api().
class CoreProServiceApi
{
public:
    virtual ~CoreProServiceApi() = default;
    virtual bool start_asset(
        const std::string& asset,
        const std::string& config,
        uint64_t delta_usec) = 0;
    virtual int create_client(
        const std::string& asset,
        const std::string& service,
        const std::string& client) = 0;
    virtual bool call(int client, const std::vector<uint8_t>& request) = 0;
    virtual bool cancel(int client) = 0;
    virtual CoreProClientEvent poll(int client) = 0;
    virtual bool response(int client, std::vector<uint8_t>& response) = 0;
    virtual bool step(uint64_t delta_usec) = 0;
    virtual void stop_asset() = 0;
};

std::shared_ptr<CoreProServiceApi> make_core_pro_service_api();

} // namespace hakoniwa::pdu::rpc
