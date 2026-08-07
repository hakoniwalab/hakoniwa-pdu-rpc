#include "hakoniwa/pdu/action/action_client_state_machine.hpp"

namespace hakoniwa::pdu::action {
namespace {

ClientTransition client_result(
    const ClientGoalContext& current,
    ClientTransitionKind kind,
    ClientTransitionError error = ClientTransitionError::NONE) noexcept
{
    return ClientTransition{kind, current, error};
}

bool is_goal_state(GoalState state) noexcept
{
    return state == GoalState::EXECUTING
        || state == GoalState::CANCELING
        || state == GoalState::FINISHING;
}

bool is_valid_client_context(const ClientGoalContext& context) noexcept
{
    if (!is_goal_state(context.state)) {
        return false;
    }
    if (context.cancel_response_pending
        && context.state != GoalState::EXECUTING) {
        return false;
    }
    if (context.state == GoalState::FINISHING) {
        return !context.result_pending
            && (context.terminal_status == TerminalStatus::SUCCEEDED
                || context.terminal_status == TerminalStatus::CANCELED
                || context.terminal_status == TerminalStatus::ABORTED);
    }
    return context.result_pending
        && context.terminal_status == TerminalStatus::UNSPECIFIED;
}

ClientTransition receive_client_result(
    const ClientGoalContext& current,
    TerminalStatus status) noexcept
{
    if (current.state == GoalState::FINISHING) {
        return client_result(
            current,
            ClientTransitionKind::NOP);
    }

    const bool allowed =
        (current.state == GoalState::EXECUTING
            && (status == TerminalStatus::SUCCEEDED
                || status == TerminalStatus::ABORTED))
        || (current.state == GoalState::CANCELING
            && (status == TerminalStatus::CANCELED
                || status == TerminalStatus::ABORTED));
    if (!allowed) {
        return client_result(
            current,
            ClientTransitionKind::NOP);
    }

    auto result = client_result(
        current,
        ClientTransitionKind::TRANSITIONED);
    result.next.state = GoalState::FINISHING;
    result.next.cancel_response_pending = false;
    result.next.result_pending = false;
    result.next.terminal_status = status;
    return result;
}

} // namespace

ClientTransition transition_client_goal(
    const ClientGoalContext& current,
    const ClientGoalEvent& event) noexcept
{
    if (!is_valid_client_context(current)) {
        return client_result(
            current,
            ClientTransitionKind::ERROR,
            ClientTransitionError::INVALID_CONTEXT);
    }

    switch (event.type) {
    case ClientGoalEventType::REQUEST_CANCEL:
        if (current.state != GoalState::EXECUTING) {
            return client_result(
                current,
                ClientTransitionKind::ERROR,
                ClientTransitionError::EVENT_NOT_ALLOWED);
        }
        if (current.cancel_response_pending) {
            return client_result(
                current,
                ClientTransitionKind::ERROR,
                ClientTransitionError::CANCEL_REQUEST_ALREADY_PENDING);
        } else {
            auto result = client_result(
                current,
                ClientTransitionKind::TRANSITIONED);
            result.next.cancel_response_pending = true;
            return result;
        }

    case ClientGoalEventType::FEEDBACK_RECEIVED:
        if (current.state == GoalState::FINISHING
            || event.feedback_sequence != current.next_feedback_sequence) {
            return client_result(
                current,
                ClientTransitionKind::NOP);
        } else {
            auto result = client_result(
                current,
                ClientTransitionKind::TRANSITIONED);
            ++result.next.next_feedback_sequence;
            return result;
        }

    case ClientGoalEventType::CANCEL_RESPONSE_ACCEPTED:
        if (current.state == GoalState::CANCELING) {
            return client_result(
                current,
                ClientTransitionKind::NOP);
        }
        if (current.state != GoalState::EXECUTING
            || !current.cancel_response_pending) {
            return client_result(
                current,
                ClientTransitionKind::NOP);
        } else {
            auto result = client_result(
                current, ClientTransitionKind::TRANSITIONED);
            result.next.state = GoalState::CANCELING;
            result.next.cancel_response_pending = false;
            return result;
        }

    case ClientGoalEventType::CANCEL_RESPONSE_REJECTED:
        if (current.state != GoalState::EXECUTING
            || !current.cancel_response_pending) {
            return client_result(
                current,
                ClientTransitionKind::NOP);
        } else {
            auto result = client_result(
                current, ClientTransitionKind::TRANSITIONED);
            result.next.cancel_response_pending = false;
            return result;
        }

    case ClientGoalEventType::RESULT_RECEIVED:
        return receive_client_result(current, event.terminal_status);

    case ClientGoalEventType::CANCEL_REQUEST_SEND_FAILED:
        if (current.state == GoalState::EXECUTING
            && current.cancel_response_pending) {
            auto result = client_result(
                current,
                ClientTransitionKind::TRANSITIONED);
            result.next.cancel_response_pending = false;
            return result;
        }
        return client_result(
            current,
            ClientTransitionKind::ERROR,
            ClientTransitionError::EVENT_NOT_ALLOWED);

    case ClientGoalEventType::CANCEL_RESPONSE_TIMEOUT:
    case ClientGoalEventType::RESULT_TIMEOUT:
    case ClientGoalEventType::TRANSPORT_DISCONNECTED:
    case ClientGoalEventType::CLIENT_SHUTDOWN_REQUESTED:
        return client_result(
            current,
            ClientTransitionKind::NOP);
    }

    return client_result(
        current,
        ClientTransitionKind::ERROR,
        ClientTransitionError::UNKNOWN_EVENT);
}

std::string_view client_transition_error_name(
    ClientTransitionError error) noexcept
{
    switch (error) {
    case ClientTransitionError::NONE:
        return "none";
    case ClientTransitionError::INVALID_CONTEXT:
        return "invalid_context";
    case ClientTransitionError::EVENT_NOT_ALLOWED:
        return "event_not_allowed";
    case ClientTransitionError::CANCEL_REQUEST_ALREADY_PENDING:
        return "cancel_request_already_pending";
    case ClientTransitionError::UNKNOWN_EVENT:
        return "unknown_event";
    }
    return "unknown_error";
}

} // namespace hakoniwa::pdu::action
