#pragma once

#include "action_types.hpp"

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <utility>

namespace hakoniwa::pdu::action {

class IActionClientEndpoint {
public:
    virtual ~IActionClientEndpoint() = default;

    virtual bool initialize(const nlohmann::json& action_config,
                            int pdu_meta_data_size) = 0;

    // Normal clients omit requested_goal_id and let the Runtime generate the
    // UUID. Protocol adapters may provide an external GoalId that must be
    // preserved. The returned handle is used for cancel and correlation.
    // timeout_usec applies only while waiting for GOAL_RESPONSE; zero disables
    // that timeout. It does not time out Result or Cancel Response delivery.
    virtual bool send_goal(const PduData& goal_pdu,
                           ClientGoalHandle& goal_handle_out,
                           std::optional<GoalId> requested_goal_id = std::nullopt,
                           std::uint64_t timeout_usec = 0) = 0;
    virtual bool send_cancel(const ClientGoalHandle& goal) = 0;
    virtual ClientEventType poll(ClientEvent& event_out) = 0;

    // Creates the generated Action Goal packet body. The Runtime writes the
    // selected GoalId into the Action header when send_goal() is called.
    virtual bool create_goal_buffer(PduData& pdu_out) = 0;
    virtual void clear_pending_events() = 0;

    const std::string& get_action_name() const { return action_name_; }
    const std::string& get_client_name() const { return client_name_; }

protected:
    IActionClientEndpoint(std::string action_name, std::string client_name,
                          std::uint64_t delta_time_usec)
        : action_name_(std::move(action_name)),
          client_name_(std::move(client_name)),
          delta_time_usec_(delta_time_usec) {}

    std::string action_name_;
    std::string client_name_;
    std::uint64_t delta_time_usec_;
};

// Result and Cancel Response timeout/retention policies are intentionally not
// part of the initial public contract.

} // namespace hakoniwa::pdu::action
