#include "hakoniwa/pdu/action/action_server_state_machine.hpp"

namespace hakoniwa::pdu::action {
namespace {

ServerTransition server_result(
    const ServerGoalContext& current,
    TransitionDecision decision,
    TransitionEffect effects = TransitionEffect::NONE,
    TransitionCommit commit = TransitionCommit::IMMEDIATE) noexcept
{
    return ServerTransition{decision, current, effects, commit};
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
            TransitionDecision::APPLICATION_API_ERROR,
            TransitionEffect::RECORD_DIAGNOSTIC);
    }

    auto result = server_result(
        current,
        TransitionDecision::ALLOW,
        TransitionEffect::COMMIT_RESULT);
    result.next.state = GoalState::FINISHING;
    result.next.cancel_decision_pending = false;
    result.next.cancel_origin = CancelOrigin::NONE;
    result.next.terminal_status = status;
    return result;
}

} // namespace

ServerTransition reduce_server_goal(
    const ServerGoalContext& current,
    ServerGoalEvent event) noexcept
{
    if (!is_valid_server_context(current)) {
        return server_result(
            current,
            TransitionDecision::INVARIANT_VIOLATION,
            TransitionEffect::RECORD_DIAGNOSTIC);
    }

    switch (event) {
    case ServerGoalEvent::PUBLISH_FEEDBACK:
        if (current.state == GoalState::EXECUTING
            || current.state == GoalState::CANCELING) {
            return server_result(
                current,
                TransitionDecision::ALLOW,
                TransitionEffect::PUBLISH_FEEDBACK);
        }
        return server_result(
            current,
            TransitionDecision::APPLICATION_API_ERROR,
            TransitionEffect::RECORD_DIAGNOSTIC);

    case ServerGoalEvent::COMPLETE_SUCCEEDED:
        return complete_server_goal(current, TerminalStatus::SUCCEEDED);
    case ServerGoalEvent::COMPLETE_CANCELED:
        return complete_server_goal(current, TerminalStatus::CANCELED);
    case ServerGoalEvent::COMPLETE_ABORTED:
        return complete_server_goal(current, TerminalStatus::ABORTED);
    case ServerGoalEvent::COMPLETE_UNSPECIFIED:
        return server_result(
            current,
            TransitionDecision::APPLICATION_API_ERROR,
            TransitionEffect::RECORD_DIAGNOSTIC);

    case ServerGoalEvent::ACCEPT_CANCEL:
    case ServerGoalEvent::REJECT_CANCEL: {
        if (current.state != GoalState::EXECUTING
            || !current.cancel_decision_pending
            || current.cancel_origin == CancelOrigin::NONE) {
            return server_result(
                current,
                TransitionDecision::APPLICATION_API_ERROR,
                TransitionEffect::RECORD_DIAGNOSTIC);
        }
        const bool accepted = event == ServerGoalEvent::ACCEPT_CANCEL;
        auto effects = TransitionEffect::NONE;
        if (current.cancel_origin == CancelOrigin::CLIENT) {
            effects = accepted
                ? TransitionEffect::SEND_CANCEL_RESPONSE_ACCEPTED
                : TransitionEffect::SEND_CANCEL_RESPONSE_REJECTED;
        }
        auto result = server_result(
            current,
            TransitionDecision::ALLOW,
            effects,
            current.cancel_origin == CancelOrigin::CLIENT
                ? TransitionCommit::AFTER_EFFECT_SUCCESS
                : TransitionCommit::IMMEDIATE);
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
                TransitionDecision::IGNORE,
                TransitionEffect::RECORD_DIAGNOSTIC);
        } else {
            auto result = server_result(
                current,
                TransitionDecision::DEFER,
                TransitionEffect::NOTIFY_CANCEL_REQUEST);
            result.next.cancel_decision_pending = true;
            result.next.cancel_origin = CancelOrigin::CLIENT;
            return result;
        }

    case ServerGoalEvent::DUPLICATE_CANCEL_REQUEST_RECEIVED:
        return server_result(
            current,
            TransitionDecision::IGNORE,
            TransitionEffect::RECORD_DIAGNOSTIC);

    case ServerGoalEvent::DUPLICATE_GOAL_REQUEST_RECEIVED:
        return server_result(
            current,
            TransitionDecision::PROTOCOL_REJECT,
            TransitionEffect::SEND_PROTOCOL_REJECT);

    case ServerGoalEvent::FEEDBACK_SEND_COMPLETED:
        return server_result(current, TransitionDecision::ALLOW);
    case ServerGoalEvent::FEEDBACK_SEND_FAILED:
        return server_result(
            current,
            TransitionDecision::DEFER,
            TransitionEffect::NOTIFY_RUNTIME_ERROR
                | TransitionEffect::DEFER_TO_POLICY);

    case ServerGoalEvent::RESULT_SEND_COMPLETED:
        if (current.state == GoalState::FINISHING) {
            return server_result(
                current,
                TransitionDecision::ALLOW,
                TransitionEffect::RELEASE_GOAL);
        }
        return server_result(
            current,
            TransitionDecision::INVARIANT_VIOLATION,
            TransitionEffect::RECORD_DIAGNOSTIC);

    case ServerGoalEvent::RESULT_SEND_FAILED:
        if (current.state == GoalState::FINISHING) {
            return server_result(
                current,
                TransitionDecision::DEFER,
                TransitionEffect::NOTIFY_RUNTIME_ERROR
                    | TransitionEffect::DEFER_TO_POLICY);
        }
        return server_result(
            current,
            TransitionDecision::INVARIANT_VIOLATION,
            TransitionEffect::RECORD_DIAGNOSTIC);

    case ServerGoalEvent::TRANSPORT_DISCONNECTED:
    case ServerGoalEvent::APPLICATION_RESPONSE_TIMEOUT:
    case ServerGoalEvent::SERVER_SHUTDOWN_REQUESTED:
        return server_result(
            current,
            TransitionDecision::DEFER,
            TransitionEffect::DEFER_TO_POLICY);

    case ServerGoalEvent::RUNTIME_CANCEL_REQUESTED:
        if (current.state == GoalState::CANCELING) {
            return server_result(current, TransitionDecision::IDEMPOTENT);
        }
        if (current.state == GoalState::FINISHING) {
            return server_result(
                current,
                TransitionDecision::IGNORE,
                TransitionEffect::RECORD_DIAGNOSTIC);
        }
        if (current.cancel_decision_pending) {
            return server_result(
                current,
                TransitionDecision::IGNORE,
                TransitionEffect::RECORD_DIAGNOSTIC);
        } else {
            auto result = server_result(
                current,
                TransitionDecision::DEFER,
                TransitionEffect::NOTIFY_CANCEL_REQUEST);
            result.next.cancel_decision_pending = true;
            result.next.cancel_origin = CancelOrigin::RUNTIME;
            return result;
        }
    }

    return server_result(
        current,
        TransitionDecision::INVARIANT_VIOLATION,
        TransitionEffect::RECORD_DIAGNOSTIC);
}

} // namespace hakoniwa::pdu::action
