#include "hakoniwa/pdu/action/action_services_mux_server.hpp"

#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/pdu/endpoint_comm_multiplexer.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>

namespace hakoniwa::pdu::action {
namespace {

enum class GoalOwnerState : std::uint8_t {
    PENDING,
    ACTIVE,
};

struct GoalOwner {
    std::string action_name;
    GoalId goal_id{};
    std::uint64_t connection_id{0};
    GoalOwnerState state{GoalOwnerState::PENDING};
};

struct ConnectionSlot {
    std::uint64_t connection_id{0};
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint;
    std::unique_ptr<ActionServicesServer> server;
    std::shared_ptr<std::atomic_bool> disconnected;
    bool orphaned{false};
    bool disconnect_processed{false};
    bool retired{false};
};

} // namespace

class ActionServicesMuxServer::Impl {
public:
    Impl(
        std::string node_id,
        std::string action_config_path,
        std::string endpoint_mux_config_path,
        std::string impl_type,
        std::uint64_t delta_time_usec,
        std::string time_source_type)
        : node_id_(std::move(node_id))
        , action_config_path_(std::move(action_config_path))
        , endpoint_mux_config_path_(std::move(endpoint_mux_config_path))
        , impl_type_(std::move(impl_type))
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
        if (node_id_.empty() || action_config_path_.empty()
            || endpoint_mux_config_path_.empty() || impl_type_.empty()
            || delta_time_usec_ == 0) {
            return false;
        }

        auto mux = std::make_unique<hakoniwa::pdu::EndpointCommMultiplexer>(
            node_id_ + "_action_mux",
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
        if (!mux_ || mux_->start() != HAKO_PDU_ERR_OK) {
            return false;
        }
        started_ = true;
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        owners_.clear();
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

    ServerEventType poll(std::string& action_name, ServerEvent& event_out)
    {
        action_name.clear();
        event_out = ServerEvent{};

        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || !mux_) {
            return ServerEventType::NONE;
        }

        accept_new_connections_();
        process_disconnected_();

        for (auto& slot : slots_) {
            if (!slot.server || slot.retired) {
                continue;
            }

            std::string candidate_action;
            ServerEvent candidate;
            const auto event_type = slot.server->poll(
                candidate_action, candidate);
            if (event_type == ServerEventType::NONE) {
                continue;
            }
            if (event_type != ServerEventType::GOAL_REQUEST) {
                action_name = std::move(candidate_action);
                event_out = std::move(candidate);
                return event_type;
            }

            if (find_owner_(candidate_action, candidate.goal.goal_id)) {
                if (!slot.server->reject_goal(
                        candidate_action, candidate.goal)) {
                    std::cerr
                        << "ERROR: Failed to send the duplicate Goal REJECT "
                        << "for Action '"
                        << candidate_action
                        << "' on a Mux connection."
                        << std::endl;
                }
                continue;
            }

            owners_.push_back(GoalOwner{
                candidate_action,
                candidate.goal.goal_id,
                slot.connection_id,
                GoalOwnerState::PENDING,
            });
            action_name = std::move(candidate_action);
            event_out = std::move(candidate);
            return event_type;
        }

        process_disconnected_();
        return ServerEventType::NONE;
    }

    bool accept_goal(
        const std::string& action_name,
        const ServerGoalHandle& goal)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* owner = find_owner_(action_name, goal.goal_id);
        if (!owner || owner->state != GoalOwnerState::PENDING) {
            return false;
        }
        auto* slot = find_slot_(owner->connection_id);
        if (!slot || !slot->server || is_disconnected_(*slot)) {
            return false;
        }
        if (!slot->server->accept_goal(action_name, goal)) {
            return false;
        }
        owner->state = GoalOwnerState::ACTIVE;
        return true;
    }

    bool reject_goal(
        const std::string& action_name,
        const ServerGoalHandle& goal)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* owner = find_owner_(action_name, goal.goal_id);
        if (!owner || owner->state != GoalOwnerState::PENDING) {
            return false;
        }
        auto* slot = find_slot_(owner->connection_id);
        if (!slot || !slot->server || is_disconnected_(*slot)
            || !slot->server->reject_goal(action_name, goal)) {
            return false;
        }
        remove_owner_(action_name, goal.goal_id);
        return true;
    }

    bool accept_cancel(
        const std::string& action_name,
        const ServerGoalHandle& goal)
    {
        return route_active_goal_(
            action_name,
            goal,
            [](ActionServicesServer& server,
               const std::string& name,
               const ServerGoalHandle& handle) {
                return server.accept_cancel(name, handle);
            });
    }

    bool reject_cancel(
        const std::string& action_name,
        const ServerGoalHandle& goal)
    {
        return route_active_goal_(
            action_name,
            goal,
            [](ActionServicesServer& server,
               const std::string& name,
               const ServerGoalHandle& handle) {
                return server.reject_cancel(name, handle);
            });
    }

    bool create_feedback_buffer(
        const std::string& action_name,
        PduData& pdu_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* server = find_any_server_();
        return server && server->create_feedback_buffer(action_name, pdu_out);
    }

    bool create_result_buffer(
        const std::string& action_name,
        PduData& pdu_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* server = find_any_server_();
        return server && server->create_result_buffer(action_name, pdu_out);
    }

    bool send_feedback(
        const std::string& action_name,
        const ServerGoalHandle& goal,
        const PduData& feedback_pdu)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* owner = find_active_owner_(action_name, goal.goal_id);
        auto* slot = owner ? find_slot_(owner->connection_id) : nullptr;
        if (!slot || !slot->server || is_disconnected_(*slot)) {
            std::cerr
                << "ERROR: Cannot send Action Feedback because the owning "
                << "Mux connection is unavailable."
                << std::endl;
            return false;
        }
        return slot->server->send_feedback(
            action_name, goal, feedback_pdu);
    }

    bool complete(
        const std::string& action_name,
        const ServerGoalHandle& goal,
        TerminalStatus status,
        const PduData& result_pdu)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* owner = find_active_owner_(action_name, goal.goal_id);
        auto* slot = owner ? find_slot_(owner->connection_id) : nullptr;
        if (!slot || !slot->server) {
            return false;
        }
        const auto connection_id = owner->connection_id;
        const bool completed = is_disconnected_(*slot)
            ? slot->server->complete_locally(
                action_name, goal, status, result_pdu)
            : slot->server->complete(
                action_name, goal, status, result_pdu);
        if (!completed) {
            return false;
        }
        remove_owner_(action_name, goal.goal_id);
        reclaim_orphaned_slot_(connection_id);
        return true;
    }

    std::size_t connected_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count_if(
            slots_.begin(),
            slots_.end(),
            [](const ConnectionSlot& slot) {
                return !is_disconnected_(slot);
            }));
    }

    std::size_t expected_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mux_ ? mux_->expected_count() : 0;
    }

    bool is_ready() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Transport readiness can become true before poll() adopts the newly
        // accepted sessions and initializes their ActionServicesServer.
        // Application readiness begins only after every expected connection
        // is represented by a live Action ConnectionSlot.
        return started_ && mux_
            && connected_slot_count_() >= mux_->expected_count();
    }

private:
    template <typename Operation>
    bool route_active_goal_(
        const std::string& action_name,
        const ServerGoalHandle& goal,
        Operation operation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* owner = find_active_owner_(action_name, goal.goal_id);
        auto* slot = owner ? find_slot_(owner->connection_id) : nullptr;
        if (!slot || !slot->server) {
            return false;
        }
        return operation(*slot->server, action_name, goal);
    }

    void accept_new_connections_()
    {
        auto endpoints = mux_->take_endpoints();
        const auto capacity = mux_->expected_count();
        for (auto& endpoint_unique : endpoints) {
            if (!endpoint_unique) {
                continue;
            }
            if (connected_slot_count_() >= capacity) {
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
            endpoint->set_on_disconnected_callback(
                [weak_disconnected](const auto&) {
                    if (auto disconnected = weak_disconnected.lock()) {
                        disconnected->store(true);
                    }
                });

            slot.server = std::make_unique<ActionServicesServer>(
                node_id_,
                action_config_path_,
                impl_type_,
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

    void process_disconnected_()
    {
        auto slot = slots_.begin();
        while (slot != slots_.end()) {
            if (!is_disconnected_(*slot) || slot->disconnect_processed) {
                ++slot;
                continue;
            }

            bool has_active_owner = false;
            auto owner = owners_.begin();
            while (owner != owners_.end()) {
                if (owner->connection_id != slot->connection_id) {
                    ++owner;
                    continue;
                }
                if (owner->state == GoalOwnerState::ACTIVE) {
                    has_active_owner = true;
                    ++owner;
                    continue;
                }

                const ServerGoalHandle pending_goal{owner->goal_id};
                if (slot->server
                    && !slot->server->discard_pending_goal(
                        owner->action_name, pending_goal)) {
                    std::cerr
                        << "ERROR: Failed to discard a pending Action Goal "
                        << "after its Mux connection disconnected."
                        << std::endl;
                }
                owner = owners_.erase(owner);
            }

            slot->disconnect_processed = true;
            if (slot->server) {
                // Drop raw packets that were never exposed to the Application.
                // Accepted Goal contexts and packet bindings remain intact.
                slot->server->stop_all_services();
            }
            if (has_active_owner) {
                slot->orphaned = true;
                if (slot->server) {
                    slot->server->notify_transport_disconnected();
                }
                ++slot;
                continue;
            }

            // Keep the disconnected Endpoint object until Mux stop. The
            // transport callback runs on its receive thread, so immediate
            // destruction can race with callback unwinding. This slot no
            // longer counts as connected and is never polled again.
            if (slot->server) {
                slot->server->clear_all_instances();
            }
            slot->retired = true;
            ++slot;
        }
    }

    std::size_t connected_slot_count_() const
    {
        return static_cast<std::size_t>(std::count_if(
            slots_.begin(),
            slots_.end(),
            [](const ConnectionSlot& slot) {
                return !is_disconnected_(slot);
            }));
    }

    void reclaim_orphaned_slot_(std::uint64_t connection_id)
    {
        const auto still_owned = std::any_of(
            owners_.begin(),
            owners_.end(),
            [connection_id](const GoalOwner& owner) {
                return owner.connection_id == connection_id;
            });
        if (still_owned) {
            return;
        }

        auto slot = std::find_if(
            slots_.begin(),
            slots_.end(),
            [connection_id](const ConnectionSlot& candidate) {
                return candidate.connection_id == connection_id
                    && candidate.orphaned;
            });
        if (slot == slots_.end()) {
            return;
        }
        if (slot->server) {
            slot->server->clear_all_instances();
        }
        slot->retired = true;
    }

    static bool is_disconnected_(const ConnectionSlot& slot)
    {
        return slot.disconnected && slot.disconnected->load();
    }

    static void close_slot_(ConnectionSlot& slot)
    {
        // The Mux owns the transport lifecycle. Join and close its receive
        // thread before releasing the Services object that also references
        // the Endpoint.
        if (slot.endpoint) {
            const auto stop_result = slot.endpoint->stop();
            const auto close_result = slot.endpoint->close();
            if (stop_result != HAKO_PDU_ERR_OK
                || close_result != HAKO_PDU_ERR_OK) {
                std::cerr
                    << "WARNING: Failed to fully stop an Action Mux "
                    << "Endpoint (stop="
                    << static_cast<int>(stop_result)
                    << ", close="
                    << static_cast<int>(close_result)
                    << ")."
                    << std::endl;
            }
        }
        if (slot.server) {
            slot.server->stop_all_services();
            slot.server->clear_all_instances();
            slot.server.reset();
        }
        if (slot.endpoint) {
            slot.endpoint.reset();
        }
    }

    ConnectionSlot* find_slot_(std::uint64_t connection_id)
    {
        auto slot = std::find_if(
            slots_.begin(),
            slots_.end(),
            [connection_id](const ConnectionSlot& candidate) {
                return candidate.connection_id == connection_id;
            });
        return slot == slots_.end() ? nullptr : &*slot;
    }

    GoalOwner* find_owner_(
        const std::string& action_name,
        const GoalId& goal_id)
    {
        auto owner = std::find_if(
            owners_.begin(),
            owners_.end(),
            [&action_name, &goal_id](const GoalOwner& candidate) {
                return candidate.action_name == action_name
                    && candidate.goal_id == goal_id;
            });
        return owner == owners_.end() ? nullptr : &*owner;
    }

    GoalOwner* find_active_owner_(
        const std::string& action_name,
        const GoalId& goal_id)
    {
        auto* owner = find_owner_(action_name, goal_id);
        return owner && owner->state == GoalOwnerState::ACTIVE
            ? owner
            : nullptr;
    }

    bool remove_owner_(
        const std::string& action_name,
        const GoalId& goal_id)
    {
        auto owner = std::find_if(
            owners_.begin(),
            owners_.end(),
            [&action_name, &goal_id](const GoalOwner& candidate) {
                return candidate.action_name == action_name
                    && candidate.goal_id == goal_id;
            });
        if (owner == owners_.end()) {
            return false;
        }
        owners_.erase(owner);
        return true;
    }

    ActionServicesServer* find_any_server_()
    {
        for (auto& slot : slots_) {
            if (slot.server) {
                return slot.server.get();
            }
        }
        return nullptr;
    }

    std::string node_id_;
    std::string action_config_path_;
    std::string endpoint_mux_config_path_;
    std::string impl_type_;
    std::uint64_t delta_time_usec_{0};
    std::string time_source_type_;

    mutable std::mutex mutex_;
    std::unique_ptr<hakoniwa::pdu::EndpointCommMultiplexer> mux_;
    std::vector<ConnectionSlot> slots_;
    std::vector<GoalOwner> owners_;
    std::uint64_t next_connection_id_{1};
    bool started_{false};
};

ActionServicesMuxServer::ActionServicesMuxServer(
    std::string node_id,
    std::string action_config_path,
    std::string endpoint_mux_config_path,
    std::string impl_type,
    std::uint64_t delta_time_usec,
    std::string time_source_type)
    : impl_(std::make_unique<Impl>(
          std::move(node_id),
          std::move(action_config_path),
          std::move(endpoint_mux_config_path),
          std::move(impl_type),
          delta_time_usec,
          std::move(time_source_type)))
{
}

ActionServicesMuxServer::~ActionServicesMuxServer() = default;

bool ActionServicesMuxServer::initialize()
{
    return impl_->initialize();
}

bool ActionServicesMuxServer::start()
{
    return impl_->start();
}

void ActionServicesMuxServer::stop()
{
    impl_->stop();
}

ServerEventType ActionServicesMuxServer::poll(
    std::string& action_name,
    ServerEvent& event_out)
{
    return impl_->poll(action_name, event_out);
}

bool ActionServicesMuxServer::accept_goal(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    return impl_->accept_goal(action_name, goal);
}

bool ActionServicesMuxServer::reject_goal(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    return impl_->reject_goal(action_name, goal);
}

bool ActionServicesMuxServer::accept_cancel(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    return impl_->accept_cancel(action_name, goal);
}

bool ActionServicesMuxServer::reject_cancel(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    return impl_->reject_cancel(action_name, goal);
}

bool ActionServicesMuxServer::create_feedback_buffer(
    const std::string& action_name,
    PduData& pdu_out)
{
    return impl_->create_feedback_buffer(action_name, pdu_out);
}

bool ActionServicesMuxServer::create_result_buffer(
    const std::string& action_name,
    PduData& pdu_out)
{
    return impl_->create_result_buffer(action_name, pdu_out);
}

bool ActionServicesMuxServer::send_feedback(
    const std::string& action_name,
    const ServerGoalHandle& goal,
    const PduData& feedback_pdu)
{
    return impl_->send_feedback(action_name, goal, feedback_pdu);
}

bool ActionServicesMuxServer::complete(
    const std::string& action_name,
    const ServerGoalHandle& goal,
    TerminalStatus status,
    const PduData& result_pdu)
{
    return impl_->complete(action_name, goal, status, result_pdu);
}

std::size_t ActionServicesMuxServer::connected_count() const
{
    return impl_->connected_count();
}

std::size_t ActionServicesMuxServer::expected_count() const
{
    return impl_->expected_count();
}

bool ActionServicesMuxServer::is_ready() const
{
    return impl_->is_ready();
}

} // namespace hakoniwa::pdu::action
