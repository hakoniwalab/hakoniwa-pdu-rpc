#pragma once

#include "action_types.hpp"

#include <cstdint>
#include <string_view>

namespace hakoniwa::pdu::action {

enum class ClientGoalEventType : std::uint8_t {
    REQUEST_CANCEL,
    FEEDBACK_RECEIVED,
    CANCEL_RESPONSE_ACCEPTED,
    CANCEL_RESPONSE_REJECTED,
    RESULT_RECEIVED,
    CANCEL_REQUEST_SEND_FAILED,
    CANCEL_RESPONSE_TIMEOUT,
    RESULT_TIMEOUT,
    TRANSPORT_DISCONNECTED,
    CLIENT_SHUTDOWN_REQUESTED,
};

struct ClientGoalEvent {
    ClientGoalEventType type{ClientGoalEventType::FEEDBACK_RECEIVED};
    TerminalStatus terminal_status{TerminalStatus::UNSPECIFIED};
    std::uint32_t feedback_sequence{0};
};

struct ClientGoalContext {
    GoalState state{GoalState::EXECUTING};
    bool cancel_response_pending{false};
    bool result_pending{true};
    std::uint32_t next_feedback_sequence{0};
    TerminalStatus terminal_status{TerminalStatus::UNSPECIFIED};
};

enum class ClientTransitionKind : std::uint8_t {
    TRANSITIONED,
    NOP,
    ERROR,
};

enum class ClientTransitionError : std::uint8_t {
    NONE,
    INVALID_CONTEXT,
    EVENT_NOT_ALLOWED,
    CANCEL_REQUEST_ALREADY_PENDING,
    UNKNOWN_EVENT,
};

struct ClientTransition {
    ClientTransitionKind kind{ClientTransitionKind::ERROR};
    ClientGoalContext next{};
    ClientTransitionError error{ClientTransitionError::INVALID_CONTEXT};

    bool transitioned() const noexcept
    {
        return kind == ClientTransitionKind::TRANSITIONED;
    }

    bool is_nop() const noexcept
    {
        return kind == ClientTransitionKind::NOP;
    }

    bool is_error() const noexcept
    {
        return kind == ClientTransitionKind::ERROR;
    }
};

ClientTransition transition_client_goal(
    const ClientGoalContext& current,
    const ClientGoalEvent& event) noexcept;

std::string_view client_transition_error_name(
    ClientTransitionError error) noexcept;

} // namespace hakoniwa::pdu::action
