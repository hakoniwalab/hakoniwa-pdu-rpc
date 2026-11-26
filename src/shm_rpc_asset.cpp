#include "hakoniwa/pdu/rpc/shm_rpc_asset.hpp"
#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"
#include <chrono>

namespace hakoniwa::pdu::rpc
{
ShmRpcAsset::ShmRpcAsset(std::string asset, std::string config, uint64_t delta, std::shared_ptr<CoreProServiceApi> api)
    : asset_name_(std::move(asset)), core_config_(std::move(config)), delta_usec_(delta), api_(std::move(api)) {}
ShmRpcAsset::~ShmRpcAsset()
{
    stop();
}
bool ShmRpcAsset::enabled() const
{
    return static_cast<bool>(api_);
}
bool ShmRpcAsset::start()
{
    if (!api_ || running_.exchange(true)) return static_cast<bool>(api_);
    thread_ = std::thread(&ShmRpcAsset::run, this);
    return true;
}
void ShmRpcAsset::stop()
{
    if (running_.exchange(false) && thread_.joinable()) thread_.join();
}
void ShmRpcAsset::add_client(const std::shared_ptr<ShmRpcClientEndpoint>& client)
{
    std::lock_guard lock(clients_mutex_);
    clients_.push_back(client);
}
void ShmRpcAsset::run()
{
    if (!api_->start_asset(asset_name_, core_config_, delta_usec_)) {
        running_ = false;
        return;
    }
    while (running_) {
        std::vector<std::shared_ptr<ShmRpcClientEndpoint>> clients;
        {
            std::lock_guard lock(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end();) if (auto p = it->lock()) {
                    clients.push_back(p);
                    ++it;
                } else it = clients_.erase(it);
        }
        for (auto& client : clients) client->asset_step(*api_);
        // This is intentionally unconditional: replies are never waited for.
        if (!api_->step(delta_usec_)) break;
    }
    api_->stop_asset();
}
} // namespace hakoniwa::pdu::rpc
