#pragma once

#include "core_pro_service_api.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hakoniwa::pdu::rpc
{
class ShmRpcClientEndpoint;

class ShmRpcAsset : public std::enable_shared_from_this<ShmRpcAsset>
{
public:
    ShmRpcAsset(
        std::string asset_name,
        std::string core_config,
        uint64_t delta_usec,
        std::shared_ptr<CoreProServiceApi> api = make_core_pro_service_api());
    ~ShmRpcAsset();
    bool start();
    void stop();
    void add_client(const std::shared_ptr<ShmRpcClientEndpoint>& client);
    bool enabled() const;
    const std::string& asset_name() const
    {
        return asset_name_;
    }

private:
    void run();

    std::string asset_name_;
    std::string core_config_;
    uint64_t delta_usec_;
    std::shared_ptr<CoreProServiceApi> api_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex clients_mutex_;
    std::vector<std::weak_ptr<ShmRpcClientEndpoint>> clients_;
};
} // namespace hakoniwa::pdu::rpc
