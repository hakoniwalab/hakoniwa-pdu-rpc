#pragma once

#include "action_types.hpp"

#include <cstdint>
#include <string_view>

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
    COMPLETE_UNSPECIFIED,
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

    // true after a Client- or Runtime-origin Cancel Request has been exposed
    // to the Application, and until the Application decides whether to accept
    // or reject that cancellation. This is an Application decision state; it
    // does not mean that the Goal has already entered CANCELING.
    bool cancel_decision_pending{false};

    // Identifies which side requested the cancellation while the Application
    // decision above is pending. A Client-origin decision requires a wire
    // response; a Runtime-origin decision does not.
    CancelOrigin cancel_origin{CancelOrigin::NONE};
    TerminalStatus terminal_status{TerminalStatus::UNSPECIFIED};
};

enum class ServerTransitionKind : std::uint8_t {
    TRANSITIONED,
    NOP,
    ERROR,
};

enum class ServerTransitionError : std::uint8_t {
    NONE,
    INVALID_CONTEXT,
    EVENT_NOT_ALLOWED,
    CANCEL_DECISION_NOT_PENDING,
    INVALID_TERMINAL_STATUS,
    UNKNOWN_EVENT,
};

struct ServerTransition {
    ServerTransitionKind kind{ServerTransitionKind::ERROR};
    ServerGoalContext next{};
    ServerTransitionError error{ServerTransitionError::INVALID_CONTEXT};

    bool transitioned() const noexcept
    {
        return kind == ServerTransitionKind::TRANSITIONED;
    }

    bool is_nop() const noexcept
    {
        return kind == ServerTransitionKind::NOP;
    }

    bool is_error() const noexcept
    {
        return kind == ServerTransitionKind::ERROR;
    }
};

ServerTransition transition_server_goal(
    const ServerGoalContext& current,
    ServerGoalEvent event) noexcept;

std::string_view server_transition_error_name(
    ServerTransitionError error) noexcept;

} // namespace hakoniwa::pdu::action
