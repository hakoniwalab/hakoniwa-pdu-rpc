#pragma once

#include "action_state_machine.hpp"
#include "action_types.hpp"

#include <cstdint>

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

struct ClientTransition {
    TransitionDecision decision{TransitionDecision::INVARIANT_VIOLATION};
    ClientGoalContext next{};
    TransitionEffect effects{TransitionEffect::NONE};
    TransitionCommit commit{TransitionCommit::IMMEDIATE};
};

ClientTransition reduce_client_goal(
    const ClientGoalContext& current,
    const ClientGoalEvent& event) noexcept;

} // namespace hakoniwa::pdu::action
