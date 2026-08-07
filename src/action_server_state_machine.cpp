#include "hakoniwa/pdu/action/action_server_state_machine.hpp"

namespace hakoniwa::pdu::action {
namespace {

ServerTransition server_result(
    const ServerGoalContext& current,
    ServerTransitionKind kind,
    ServerTransitionError error = ServerTransitionError::NONE) noexcept
{
    return ServerTransition{kind, current, error};
}

bool is_goal_state(GoalState state) noexcept
{
    return state == GoalState::EXECUTING
        || state == GoalState::CANCELING
        || state == GoalState::FINISHING;
}

bool is_valid_server_context(const ServerGoalContext& context) noexcept
{
    if (!is_goal_state(context.state)) {
        return false;
    }
    if (context.cancel_decision_pending
        != (context.cancel_origin != CancelOrigin::NONE)) {
        return false;
    }
    if (context.cancel_decision_pending
        && context.state != GoalState::EXECUTING) {
        return false;
    }
    if (context.state == GoalState::FINISHING) {
        return context.terminal_status == TerminalStatus::SUCCEEDED
            || context.terminal_status == TerminalStatus::CANCELED
            || context.terminal_status == TerminalStatus::ABORTED;
    }
    return context.terminal_status == TerminalStatus::UNSPECIFIED;
}

ServerTransition complete_server_goal(
    const ServerGoalContext& current,
    TerminalStatus status) noexcept
{
    const bool allowed =
        (current.state == GoalState::EXECUTING
            && (status == TerminalStatus::SUCCEEDED
                || status == TerminalStatus::ABORTED))
        || (current.state == GoalState::CANCELING
            && (status == TerminalStatus::CANCELED
                || status == TerminalStatus::ABORTED));
    if (!allowed) {
        return server_result(
            current,
            ServerTransitionKind::ERROR,
            status == TerminalStatus::UNSPECIFIED
                ? ServerTransitionError::INVALID_TERMINAL_STATUS
                : ServerTransitionError::EVENT_NOT_ALLOWED);
    }

    auto result = server_result(
        current,
        ServerTransitionKind::TRANSITIONED);
    result.next.state = GoalState::FINISHING;
    result.next.cancel_decision_pending = false;
    result.next.cancel_origin = CancelOrigin::NONE;
    result.next.terminal_status = status;
    return result;
}

} // namespace

ServerTransition transition_server_goal(
    const ServerGoalContext& current,
    ServerGoalEvent event) noexcept
{
    if (!is_valid_server_context(current)) {
        return server_result(
            current,
            ServerTransitionKind::ERROR,
            ServerTransitionError::INVALID_CONTEXT);
    }

    switch (event) {
    case ServerGoalEvent::PUBLISH_FEEDBACK:
        if (current.state == GoalState::EXECUTING
            || current.state == GoalState::CANCELING) {
            return server_result(
                current,
                ServerTransitionKind::NOP);
        }
        return server_result(
            current,
            ServerTransitionKind::ERROR,
            ServerTransitionError::EVENT_NOT_ALLOWED);

    case ServerGoalEvent::COMPLETE_SUCCEEDED:
        return complete_server_goal(current, TerminalStatus::SUCCEEDED);
    case ServerGoalEvent::COMPLETE_CANCELED:
        return complete_server_goal(current, TerminalStatus::CANCELED);
    case ServerGoalEvent::COMPLETE_ABORTED:
        return complete_server_goal(current, TerminalStatus::ABORTED);
    case ServerGoalEvent::COMPLETE_UNSPECIFIED:
        return server_result(
            current,
            ServerTransitionKind::ERROR,
            ServerTransitionError::INVALID_TERMINAL_STATUS);

    case ServerGoalEvent::ACCEPT_CANCEL:
    case ServerGoalEvent::REJECT_CANCEL: {
        if (current.state != GoalState::EXECUTING) {
            return server_result(
                current,
                ServerTransitionKind::ERROR,
                ServerTransitionError::EVENT_NOT_ALLOWED);
        }
        if (!current.cancel_decision_pending
            || current.cancel_origin == CancelOrigin::NONE) {
            return server_result(
                current,
                ServerTransitionKind::ERROR,
                ServerTransitionError::CANCEL_DECISION_NOT_PENDING);
        }
        const bool accepted = event == ServerGoalEvent::ACCEPT_CANCEL;
        auto result = server_result(
            current,
            ServerTransitionKind::TRANSITIONED);
        result.next.state = accepted
            ? GoalState::CANCELING
            : GoalState::EXECUTING;
        result.next.cancel_decision_pending = false;
        result.next.cancel_origin = CancelOrigin::NONE;
        return result;
    }

    case ServerGoalEvent::CANCEL_REQUEST_RECEIVED:
        if (current.state != GoalState::EXECUTING
            || current.cancel_decision_pending) {
            return server_result(
                current,
                ServerTransitionKind::NOP);
        } else {
            auto result = server_result(
                current,
                ServerTransitionKind::TRANSITIONED);
            result.next.cancel_decision_pending = true;
            result.next.cancel_origin = CancelOrigin::CLIENT;
            return result;
        }

    case ServerGoalEvent::DUPLICATE_CANCEL_REQUEST_RECEIVED:
        return server_result(
            current,
            ServerTransitionKind::NOP);

    case ServerGoalEvent::DUPLICATE_GOAL_REQUEST_RECEIVED:
        return server_result(
            current,
            ServerTransitionKind::NOP);

    case ServerGoalEvent::FEEDBACK_SEND_COMPLETED:
        return server_result(current, ServerTransitionKind::NOP);
    case ServerGoalEvent::FEEDBACK_SEND_FAILED:
        return server_result(
            current,
            ServerTransitionKind::NOP);

    case ServerGoalEvent::RESULT_SEND_COMPLETED:
        if (current.state == GoalState::FINISHING) {
            return server_result(
                current,
                ServerTransitionKind::NOP);
        }
        return server_result(
            current,
            ServerTransitionKind::ERROR,
            ServerTransitionError::EVENT_NOT_ALLOWED);

    case ServerGoalEvent::RESULT_SEND_FAILED:
        if (current.state == GoalState::FINISHING) {
            return server_result(
                current,
                ServerTransitionKind::NOP);
        }
        return server_result(
            current,
            ServerTransitionKind::ERROR,
            ServerTransitionError::EVENT_NOT_ALLOWED);

    case ServerGoalEvent::TRANSPORT_DISCONNECTED:
    case ServerGoalEvent::APPLICATION_RESPONSE_TIMEOUT:
    case ServerGoalEvent::SERVER_SHUTDOWN_REQUESTED:
        return server_result(
            current,
            ServerTransitionKind::NOP);

    case ServerGoalEvent::RUNTIME_CANCEL_REQUESTED:
        if (current.state == GoalState::CANCELING) {
            return server_result(current, ServerTransitionKind::NOP);
        }
        if (current.state == GoalState::FINISHING) {
            return server_result(
                current,
                ServerTransitionKind::NOP);
        }
        if (current.cancel_decision_pending) {
            if (current.cancel_origin == CancelOrigin::RUNTIME) {
                return server_result(
                    current,
                    ServerTransitionKind::NOP);
            }
            // A transport disconnect makes the pending Client Cancel wire
            // response impossible. Preserve the Application decision, but
            // reclassify it as Runtime-origin so accept/reject does not try
            // to send on the dead connection.
            auto result = server_result(
                current,
                ServerTransitionKind::TRANSITIONED);
            result.next.cancel_origin = CancelOrigin::RUNTIME;
            return result;
        } else {
            auto result = server_result(
                current,
                ServerTransitionKind::TRANSITIONED);
            result.next.cancel_decision_pending = true;
            result.next.cancel_origin = CancelOrigin::RUNTIME;
            return result;
        }
    }

    return server_result(
        current,
        ServerTransitionKind::ERROR,
        ServerTransitionError::UNKNOWN_EVENT);
}

std::string_view server_transition_error_name(
    ServerTransitionError error) noexcept
{
    switch (error) {
    case ServerTransitionError::NONE:
        return "none";
    case ServerTransitionError::INVALID_CONTEXT:
        return "invalid_context";
    case ServerTransitionError::EVENT_NOT_ALLOWED:
        return "event_not_allowed";
    case ServerTransitionError::CANCEL_DECISION_NOT_PENDING:
        return "cancel_decision_not_pending";
    case ServerTransitionError::INVALID_TERMINAL_STATUS:
        return "invalid_terminal_status";
    case ServerTransitionError::UNKNOWN_EVENT:
        return "unknown_event";
    }
    return "unknown_error";
}

} // namespace hakoniwa::pdu::action
