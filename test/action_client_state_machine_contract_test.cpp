#include "hakoniwa/pdu/action/action_client_state_machine.hpp"

#include <gtest/gtest.h>

#include <array>

namespace action = hakoniwa::pdu::action;

namespace {

using TK = action::ClientTransitionKind;
using TE = action::ClientTransitionError;

action::ClientGoalContext client_context(
    action::GoalState state,
    bool cancel_pending = false)
{
    return action::ClientGoalContext{
        state,
        cancel_pending,
        state != action::GoalState::FINISHING,
        0,
        state == action::GoalState::FINISHING
            ? action::TerminalStatus::SUCCEEDED
            : action::TerminalStatus::UNSPECIFIED,
    };
}

} // namespace

TEST(ActionClientStateMachineContract, CancelRequestAndResponseTransitionContext)
{
    const auto requested = action::transition_client_goal(
        client_context(action::GoalState::EXECUTING),
        {action::ClientGoalEventType::REQUEST_CANCEL});
    ASSERT_TRUE(requested.transitioned());
    EXPECT_TRUE(requested.next.cancel_response_pending);
    EXPECT_EQ(requested.next.state, action::GoalState::EXECUTING);

    const auto duplicate_request = action::transition_client_goal(
        requested.next,
        {action::ClientGoalEventType::REQUEST_CANCEL});
    EXPECT_TRUE(duplicate_request.is_error());
    EXPECT_EQ(
        duplicate_request.error,
        TE::CANCEL_REQUEST_ALREADY_PENDING);

    const auto accepted = action::transition_client_goal(
        requested.next,
        {action::ClientGoalEventType::CANCEL_RESPONSE_ACCEPTED});
    EXPECT_TRUE(accepted.transitioned());
    EXPECT_EQ(accepted.next.state, action::GoalState::CANCELING);
    EXPECT_FALSE(accepted.next.cancel_response_pending);

    const auto rejected = action::transition_client_goal(
        requested.next,
        {action::ClientGoalEventType::CANCEL_RESPONSE_REJECTED});
    EXPECT_TRUE(rejected.transitioned());
    EXPECT_EQ(rejected.next.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(rejected.next.cancel_response_pending);
}

TEST(ActionClientStateMachineContract, FeedbackSequenceTransitionsClientContext)
{
    const auto first = action::transition_client_goal(
        client_context(action::GoalState::EXECUTING),
        {action::ClientGoalEventType::FEEDBACK_RECEIVED,
            action::TerminalStatus::UNSPECIFIED,
            0});
    ASSERT_TRUE(first.transitioned());
    EXPECT_EQ(first.next.next_feedback_sequence, 1U);

    const auto duplicate = action::transition_client_goal(
        first.next,
        {action::ClientGoalEventType::FEEDBACK_RECEIVED,
            action::TerminalStatus::UNSPECIFIED,
            0});
    EXPECT_TRUE(duplicate.is_nop());
    EXPECT_EQ(duplicate.next.next_feedback_sequence, 1U);
}

TEST(ActionClientStateMachineContract, ResultStatusFollowsObservedServerState)
{
    struct Case {
        action::GoalState state;
        action::TerminalStatus terminal;
        TK kind;
    };
    constexpr std::array cases{
        Case{action::GoalState::EXECUTING,
            action::TerminalStatus::SUCCEEDED,
            TK::TRANSITIONED},
        Case{action::GoalState::EXECUTING,
            action::TerminalStatus::CANCELED,
            TK::NOP},
        Case{action::GoalState::EXECUTING,
            action::TerminalStatus::ABORTED,
            TK::TRANSITIONED},
        Case{action::GoalState::CANCELING,
            action::TerminalStatus::SUCCEEDED,
            TK::NOP},
        Case{action::GoalState::CANCELING,
            action::TerminalStatus::CANCELED,
            TK::TRANSITIONED},
        Case{action::GoalState::CANCELING,
            action::TerminalStatus::ABORTED,
            TK::TRANSITIONED},
    };

    for (const auto& test_case : cases) {
        const auto transition = action::transition_client_goal(
            client_context(test_case.state),
            {action::ClientGoalEventType::RESULT_RECEIVED,
                test_case.terminal});
        EXPECT_EQ(transition.kind, test_case.kind);
        EXPECT_EQ(
            transition.next.state,
            test_case.kind == TK::TRANSITIONED
                ? action::GoalState::FINISHING
                : test_case.state);
    }
}

TEST(ActionClientStateMachineContract, ResultMayWinWhileCancelResponseIsPending)
{
    const auto transition = action::transition_client_goal(
        client_context(action::GoalState::EXECUTING, true),
        {action::ClientGoalEventType::RESULT_RECEIVED,
            action::TerminalStatus::SUCCEEDED});
    EXPECT_TRUE(transition.transitioned());
    EXPECT_EQ(transition.next.state, action::GoalState::FINISHING);
    EXPECT_FALSE(transition.next.cancel_response_pending);
}

TEST(ActionClientStateMachineContract, RuntimeFailuresDoNotInventTerminalStatus)
{
    const auto send_failed = action::transition_client_goal(
        client_context(action::GoalState::EXECUTING, true),
        {action::ClientGoalEventType::CANCEL_REQUEST_SEND_FAILED});
    EXPECT_TRUE(send_failed.transitioned());
    EXPECT_FALSE(send_failed.next.cancel_response_pending);
    EXPECT_EQ(
        send_failed.next.terminal_status,
        action::TerminalStatus::UNSPECIFIED);

    for (const auto event : {
             action::ClientGoalEventType::CANCEL_RESPONSE_TIMEOUT,
             action::ClientGoalEventType::RESULT_TIMEOUT,
             action::ClientGoalEventType::TRANSPORT_DISCONNECTED,
             action::ClientGoalEventType::CLIENT_SHUTDOWN_REQUESTED}) {
        const auto transition = action::transition_client_goal(
            client_context(action::GoalState::EXECUTING), {event});
        EXPECT_TRUE(transition.is_nop());
        EXPECT_EQ(
            transition.next.terminal_status,
            action::TerminalStatus::UNSPECIFIED);
    }
}

TEST(ActionClientStateMachineContract, InvalidContextReturnsReason)
{
    const auto invalid_state = action::transition_client_goal(
        client_context(action::GoalState::UNSPECIFIED),
        {action::ClientGoalEventType::REQUEST_CANCEL});
    EXPECT_TRUE(invalid_state.is_error());
    EXPECT_EQ(invalid_state.error, TE::INVALID_CONTEXT);
    EXPECT_EQ(
        action::client_transition_error_name(invalid_state.error),
        "invalid_context");

    auto impossible = client_context(action::GoalState::CANCELING);
    impossible.cancel_response_pending = true;
    const auto transition = action::transition_client_goal(
        impossible,
        {action::ClientGoalEventType::FEEDBACK_RECEIVED});
    EXPECT_TRUE(transition.is_error());
    EXPECT_EQ(transition.error, TE::INVALID_CONTEXT);
}

TEST(ActionClientStateMachineContract, InvalidCancelOperationReturnsReason)
{
    const auto request_while_canceling = action::transition_client_goal(
        client_context(action::GoalState::CANCELING),
        {action::ClientGoalEventType::REQUEST_CANCEL});
    EXPECT_TRUE(request_while_canceling.is_error());
    EXPECT_EQ(request_while_canceling.error, TE::EVENT_NOT_ALLOWED);

    const auto send_failure_without_pending = action::transition_client_goal(
        client_context(action::GoalState::EXECUTING),
        {action::ClientGoalEventType::CANCEL_REQUEST_SEND_FAILED});
    EXPECT_TRUE(send_failure_without_pending.is_error());
    EXPECT_EQ(send_failure_without_pending.error, TE::EVENT_NOT_ALLOWED);
}
