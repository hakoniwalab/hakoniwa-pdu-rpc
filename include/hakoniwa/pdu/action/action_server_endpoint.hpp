#pragma once

#include "action_types.hpp"

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <utility>

namespace hakoniwa::pdu::action {

class IActionServerEndpoint {
public:
    virtual ~IActionServerEndpoint() = default;

    virtual bool initialize(const nlohmann::json& action_config) = 0;

    virtual ServerEventType poll(ServerEvent& event_out) = 0;

    // event_token is one-shot and is consumed by exactly one accept/reject call.
    // Accepting a goal creates a long-lived goal_token used for feedback and
    // terminal completion.
    virtual bool accept_goal(const ServerGoalHandle& goal) = 0;
    virtual bool reject_goal(const ServerGoalHandle& goal) = 0;
    virtual bool accept_cancel(const ServerGoalHandle& goal) = 0;
    virtual bool reject_cancel(const ServerGoalHandle& goal) = 0;

    virtual bool send_feedback(const ServerGoalHandle& goal,
        const PduData& feedback_pdu) = 0;
    virtual bool complete(const ServerGoalHandle& goal,
                          TerminalStatus status,
                          const PduData& result_pdu) = 0;

    virtual void clear_pending_events() = 0;
    const std::string& get_action_name() const { return action_name_; }

protected:
    IActionServerEndpoint(std::string action_name,
                          std::uint64_t delta_time_usec)
        : action_name_(std::move(action_name)),
          delta_time_usec_(delta_time_usec) {}

    std::string action_name_;
    std::uint64_t delta_time_usec_;
};

// Invariant for the implementation:
// COMPLETE_SUCCEEDED and ACCEPT_CANCEL must commit atomically per goal. If the
// Result wins, the pending cancel token is invalidated and no Cancel Response
// is emitted. See docs/design/action/10-cancel-result-race.md.

} // namespace hakoniwa::pdu::action
