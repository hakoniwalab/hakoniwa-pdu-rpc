#pragma once

#include "action_types.hpp"

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace hakoniwa::pdu::action {

class IActionClientEndpoint {
public:
    virtual ~IActionClientEndpoint() = default;

    virtual bool initialize(const nlohmann::json& action_config,
                            int pdu_meta_data_size) = 0;

    // One endpoint may own multiple active goals. goal_id is the correlation
    // key; a single in-flight request restriction must not be introduced.
    virtual bool send_goal(const GoalId& goal_id, const PduData& goal_pdu,
                           std::uint64_t timeout_usec) = 0;
    virtual bool send_cancel(const GoalId& goal_id) = 0;
    virtual ClientEventType poll(ClientEvent& event_out) = 0;
    virtual bool create_goal_buffer(const GoalId& goal_id,
                                    PduData& pdu_out) = 0;
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

// TODO(codex): specify timeout semantics per goal and terminal-event retention
// after the first native implementation proves the required bookkeeping.

} // namespace hakoniwa::pdu::action
