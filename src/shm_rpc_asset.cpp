#include "hakoniwa/pdu/rpc/shm_rpc_asset.hpp"
#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"
#include <chrono>

namespace hakoniwa::pdu::rpc
{
ShmRpcAsset::ShmRpcAsset(
    std::string asset,
    std::string core_config,
    std::string service_config,
    uint64_t delta,
    std::shared_ptr<CoreProServiceApi> api)
    : asset_name_(std::move(asset)), core_config_(std::move(core_config)),
      service_config_(std::move(service_config)), delta_usec_(delta), api_(std::move(api)) {}
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
    if (!api_->initialize_asset(asset_name_, core_config_, service_config_)) {
        running_ = false;
        return;
    }
    while (running_) {
        const auto event = api_->asset_event(asset_name_);
        if (event == CoreProAssetEvent::error) break;
        std::vector<std::shared_ptr<ShmRpcClientEndpoint>> clients;
        {
            std::lock_guard lock(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end();) if (auto p = it->lock()) {
                    clients.push_back(p);
                    ++it;
                } else it = clients_.erase(it);
        }
        if (event == CoreProAssetEvent::start) {
            if (!api_->pdu_created()) break;
            bool clients_ready = true;
            for (auto& client : clients) clients_ready &= client->asset_started(*api_);
            if (!clients_ready || !api_->start_feedback(asset_name_)) break;
        } else if (event == CoreProAssetEvent::stop || event == CoreProAssetEvent::reset) {
            for (auto& client : clients) client->asset_stopped();
            if (event == CoreProAssetEvent::stop) api_->stop_feedback(asset_name_);
            else api_->reset_feedback(asset_name_);
        } else if (api_->running()) {
            for (auto& client : clients) client->asset_step(*api_);
            api_->advance_time(asset_name_, delta_usec_);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    api_->stop_asset();
}
} // namespace hakoniwa::pdu::rpc
