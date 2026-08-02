#include "hakoniwa/pdu/rpc/rpc_services_mux_server.hpp"

#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/pdu/endpoint_comm_multiplexer.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace hakoniwa::pdu::rpc {
namespace {

struct ConnectionSlot {
    std::uint64_t connection_id{0};
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint;
    std::unique_ptr<RpcServicesServer> server;
    std::shared_ptr<std::atomic_bool> disconnected;
};

} // namespace

class RpcServicesMuxServer::Impl {
public:
    Impl(
        std::string node_id,
        std::string impl_type,
        std::string service_config_path,
        std::string endpoint_mux_config_path,
        std::uint64_t delta_time_usec,
        std::string time_source_type)
        : node_id_(std::move(node_id))
        , impl_type_(std::move(impl_type))
        , service_config_path_(std::move(service_config_path))
        , endpoint_mux_config_path_(std::move(endpoint_mux_config_path))
        , delta_time_usec_(delta_time_usec)
        , time_source_type_(std::move(time_source_type))
    {
    }

    ~Impl()
    {
        stop();
    }

    bool initialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mux_) {
            return true;
        }
        if (node_id_.empty() || impl_type_.empty() || service_config_path_.empty()
            || endpoint_mux_config_path_.empty() || delta_time_usec_ == 0) {
            return false;
        }

        auto mux = std::make_unique<hakoniwa::pdu::EndpointCommMultiplexer>(
            node_id_ + "_rpc_mux",
            HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
        if (mux->open(endpoint_mux_config_path_) != HAKO_PDU_ERR_OK) {
            return false;
        }
        if (mux->expected_count() == 0) {
            (void)mux->close();
            return false;
        }
        mux_ = std::move(mux);
        return true;
    }

    bool start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return true;
        }
        if (!mux_) {
            return false;
        }
        if (mux_->start() != HAKO_PDU_ERR_OK) {
            return false;
        }
        started_ = true;
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : slots_) {
            close_slot_(slot);
        }
        slots_.clear();

        if (mux_) {
            (void)mux_->stop();
            (void)mux_->close();
            mux_.reset();
        }
        started_ = false;
    }

    ServerEventType poll(RpcMuxRequest& request)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || !mux_) {
            return ServerEventType::NONE;
        }

        accept_new_connections_();
        cleanup_disconnected_();

        for (auto& slot : slots_) {
            if (!slot.server) {
                continue;
            }
            RpcRequest candidate;
            const auto event = slot.server->poll(candidate);
            if (event != ServerEventType::NONE) {
                request.connection_id = slot.connection_id;
                request.request = std::move(candidate);
                return event;
            }
        }

        cleanup_disconnected_();
        return ServerEventType::NONE;
    }

    bool create_reply_buffer(
        const RpcMuxRequest& request,
        Hako_uint8 status,
        Hako_int32 result_code,
        PduData& pdu)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = find_slot_(request.connection_id);
        if (!slot || !slot->server) {
            return false;
        }
        slot->server->create_reply_buffer(
            request.request.header, status, result_code, pdu);
        return !pdu.empty();
    }

    bool send_reply(const RpcMuxRequest& request, const PduData& pdu)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = find_slot_(request.connection_id);
        if (!slot || !slot->server) {
            return false;
        }
        slot->server->send_reply(request.request.header, pdu);
        return true;
    }

    bool send_cancel_reply(const RpcMuxRequest& request, const PduData& pdu)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = find_slot_(request.connection_id);
        if (!slot || !slot->server) {
            return false;
        }
        slot->server->send_cancel_reply(request.request.header, pdu);
        return true;
    }

    std::size_t connected_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_.size();
    }

    std::size_t expected_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mux_ ? mux_->expected_count() : 0;
    }

    bool is_ready() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return started_ && mux_ && mux_->is_ready();
    }

private:
    void accept_new_connections_()
    {
        auto endpoints = mux_->take_endpoints();
        const auto capacity = mux_->expected_count();
        for (auto& endpoint_unique : endpoints) {
            if (!endpoint_unique) {
                continue;
            }
            if (slots_.size() >= capacity) {
                (void)endpoint_unique->stop();
                (void)endpoint_unique->close();
                continue;
            }

            auto endpoint = std::shared_ptr<hakoniwa::pdu::Endpoint>(
                std::move(endpoint_unique));
            ConnectionSlot slot;
            slot.connection_id = next_connection_id_++;
            slot.endpoint = endpoint;
            slot.disconnected = std::make_shared<std::atomic_bool>(false);

            std::weak_ptr<std::atomic_bool> weak_disconnected = slot.disconnected;
            endpoint->set_on_disconnected_callback([weak_disconnected](const auto&) {
                if (auto disconnected = weak_disconnected.lock()) {
                    disconnected->store(true);
                }
            });

            slot.server = std::make_unique<RpcServicesServer>(
                node_id_,
                impl_type_,
                service_config_path_,
                delta_time_usec_,
                time_source_type_);
            if (!slot.server->initialize_services(endpoint)
                || !slot.server->start_all_services()) {
                slot.server.reset();
                (void)endpoint->stop();
                (void)endpoint->close();
                continue;
            }
            slots_.push_back(std::move(slot));
        }
    }

    void cleanup_disconnected_()
    {
        auto it = slots_.begin();
        while (it != slots_.end()) {
            if (!it->disconnected || !it->disconnected->load()) {
                ++it;
                continue;
            }
            close_slot_(*it);
            it = slots_.erase(it);
        }
    }

    static void close_slot_(ConnectionSlot& slot)
    {
        if (slot.server) {
            slot.server->stop_all_services();
            slot.server.reset();
        }
        if (slot.endpoint) {
            (void)slot.endpoint->stop();
            (void)slot.endpoint->close();
            slot.endpoint.reset();
        }
    }

    ConnectionSlot* find_slot_(std::uint64_t connection_id)
    {
        auto it = std::find_if(
            slots_.begin(),
            slots_.end(),
            [connection_id](const ConnectionSlot& slot) {
                return slot.connection_id == connection_id;
            });
        return it == slots_.end() ? nullptr : &*it;
    }

    std::string node_id_;
    std::string impl_type_;
    std::string service_config_path_;
    std::string endpoint_mux_config_path_;
    std::uint64_t delta_time_usec_{0};
    std::string time_source_type_;

    mutable std::mutex mutex_;
    std::unique_ptr<hakoniwa::pdu::EndpointCommMultiplexer> mux_;
    std::vector<ConnectionSlot> slots_;
    std::uint64_t next_connection_id_{1};
    bool started_{false};
};

RpcServicesMuxServer::RpcServicesMuxServer(
    std::string node_id,
    std::string impl_type,
    std::string service_config_path,
    std::string endpoint_mux_config_path,
    std::uint64_t delta_time_usec,
    std::string time_source_type)
    : impl_(std::make_unique<Impl>(
          std::move(node_id),
          std::move(impl_type),
          std::move(service_config_path),
          std::move(endpoint_mux_config_path),
          delta_time_usec,
          std::move(time_source_type)))
{
}

RpcServicesMuxServer::~RpcServicesMuxServer() = default;

bool RpcServicesMuxServer::initialize()
{
    return impl_->initialize();
}

bool RpcServicesMuxServer::start()
{
    return impl_->start();
}

void RpcServicesMuxServer::stop()
{
    impl_->stop();
}

ServerEventType RpcServicesMuxServer::poll(RpcMuxRequest& request)
{
    return impl_->poll(request);
}

bool RpcServicesMuxServer::create_reply_buffer(
    const RpcMuxRequest& request,
    Hako_uint8 status,
    Hako_int32 result_code,
    PduData& pdu)
{
    return impl_->create_reply_buffer(request, status, result_code, pdu);
}

bool RpcServicesMuxServer::send_reply(
    const RpcMuxRequest& request,
    const PduData& pdu)
{
    return impl_->send_reply(request, pdu);
}

bool RpcServicesMuxServer::send_cancel_reply(
    const RpcMuxRequest& request,
    const PduData& pdu)
{
    return impl_->send_cancel_reply(request, pdu);
}

std::size_t RpcServicesMuxServer::connected_count() const
{
    return impl_->connected_count();
}

std::size_t RpcServicesMuxServer::expected_count() const
{
    return impl_->expected_count();
}

bool RpcServicesMuxServer::is_ready() const
{
    return impl_->is_ready();
}

} // namespace hakoniwa::pdu::rpc
