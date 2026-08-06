#pragma once

#include "action_state_machine.hpp"
#include "action_types.hpp"

#include <cstdint>

namespace hakoniwa::pdu::action {

enum class CancelOrigin : std::uint8_t {
    NONE,
    CLIENT,
    RUNTIME,
};

enum class ServerGoalEvent : std::uint8_t {
    PUBLISH_FEEDBACK,
    COMPLETE_SUCCEEDED,
    COMPLETE_CANCELED,
    COMPLETE_ABORTED,
    ACCEPT_CANCEL,
    REJECT_CANCEL,
    CANCEL_REQUEST_RECEIVED,
    DUPLICATE_CANCEL_REQUEST_RECEIVED,
    DUPLICATE_GOAL_REQUEST_RECEIVED,
    FEEDBACK_SEND_COMPLETED,
    FEEDBACK_SEND_FAILED,
    RESULT_SEND_COMPLETED,
    RESULT_SEND_FAILED,
    TRANSPORT_DISCONNECTED,
    RUNTIME_CANCEL_REQUESTED,
    APPLICATION_RESPONSE_TIMEOUT,
    SERVER_SHUTDOWN_REQUESTED,
};

struct ServerGoalContext {
    GoalState state{GoalState::EXECUTING};
    bool cancel_decision_pending{false};
    CancelOrigin cancel_origin{CancelOrigin::NONE};
    TerminalStatus terminal_status{TerminalStatus::UNSPECIFIED};
};

struct ServerTransition {
    TransitionDecision decision{TransitionDecision::INVARIANT_VIOLATION};
    ServerGoalContext next{};
    TransitionEffect effects{TransitionEffect::NONE};
    TransitionCommit commit{TransitionCommit::IMMEDIATE};
};

ServerTransition reduce_server_goal(
    const ServerGoalContext& current,
    ServerGoalEvent event) noexcept;

} // namespace hakoniwa::pdu::action
