#include "hakoniwa/pdu/action/action_client_state_machine.hpp"

#include <gtest/gtest.h>

#include <array>

namespace action = hakoniwa::pdu::action;

namespace {

using TD = action::TransitionDecision;
using TE = action::TransitionEffect;

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

TEST(ActionClientStateMachineContract, CancelRequestAndResponseFollowPendingContext)
{
    const auto requested = action::reduce_client_goal(
        client_context(action::GoalState::EXECUTING),
        {action::ClientGoalEventType::REQUEST_CANCEL});
    ASSERT_EQ(requested.decision, TD::ALLOW);
    EXPECT_TRUE(requested.next.cancel_response_pending);
    EXPECT_EQ(requested.next.state, action::GoalState::EXECUTING);
    EXPECT_TRUE(action::has_effect(
        requested.effects, TE::SEND_CANCEL_REQUEST));
    EXPECT_EQ(
        requested.commit,
        action::TransitionCommit::AFTER_EFFECT_SUCCESS);

    const auto duplicate_request = action::reduce_client_goal(
        requested.next,
        {action::ClientGoalEventType::REQUEST_CANCEL});
    EXPECT_EQ(duplicate_request.decision, TD::APPLICATION_API_ERROR);

    const auto accepted = action::reduce_client_goal(
        requested.next,
        {action::ClientGoalEventType::CANCEL_RESPONSE_ACCEPTED});
    EXPECT_EQ(accepted.decision, TD::ALLOW);
    EXPECT_EQ(accepted.next.state, action::GoalState::CANCELING);
    EXPECT_FALSE(accepted.next.cancel_response_pending);

    const auto rejected = action::reduce_client_goal(
        requested.next,
        {action::ClientGoalEventType::CANCEL_RESPONSE_REJECTED});
    EXPECT_EQ(rejected.decision, TD::ALLOW);
    EXPECT_EQ(rejected.next.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(rejected.next.cancel_response_pending);
}

TEST(ActionClientStateMachineContract, FeedbackSequenceIsPartOfPureClientContext)
{
    const auto first = action::reduce_client_goal(
        client_context(action::GoalState::EXECUTING),
        {action::ClientGoalEventType::FEEDBACK_RECEIVED,
            action::TerminalStatus::UNSPECIFIED,
            0});
    ASSERT_EQ(first.decision, TD::ALLOW);
    EXPECT_EQ(first.next.next_feedback_sequence, 1U);
    EXPECT_TRUE(action::has_effect(first.effects, TE::DELIVER_FEEDBACK));

    const auto duplicate = action::reduce_client_goal(
        first.next,
        {action::ClientGoalEventType::FEEDBACK_RECEIVED,
            action::TerminalStatus::UNSPECIFIED,
            0});
    EXPECT_EQ(duplicate.decision, TD::IGNORE);
    EXPECT_EQ(duplicate.next.next_feedback_sequence, 1U);
}

TEST(ActionClientStateMachineContract, ResultStatusFollowsObservedServerState)
{
    struct Case {
        action::GoalState state;
        action::TerminalStatus terminal;
        TD decision;
    };
    constexpr std::array cases{
        Case{action::GoalState::EXECUTING,
            action::TerminalStatus::SUCCEEDED,
            TD::ALLOW},
        Case{action::GoalState::EXECUTING,
            action::TerminalStatus::CANCELED,
            TD::IGNORE},
        Case{action::GoalState::EXECUTING,
            action::TerminalStatus::ABORTED,
            TD::ALLOW},
        Case{action::GoalState::CANCELING,
            action::TerminalStatus::SUCCEEDED,
            TD::IGNORE},
        Case{action::GoalState::CANCELING,
            action::TerminalStatus::CANCELED,
            TD::ALLOW},
        Case{action::GoalState::CANCELING,
            action::TerminalStatus::ABORTED,
            TD::ALLOW},
    };

    for (const auto& test_case : cases) {
        const auto transition = action::reduce_client_goal(
            client_context(test_case.state),
            {action::ClientGoalEventType::RESULT_RECEIVED,
                test_case.terminal});
        EXPECT_EQ(transition.decision, test_case.decision);
        EXPECT_EQ(
            transition.next.state,
            test_case.decision == TD::ALLOW
                ? action::GoalState::FINISHING
                : test_case.state);
        EXPECT_EQ(
            action::has_effect(transition.effects, TE::DELIVER_RESULT),
            test_case.decision == TD::ALLOW);
    }
}

TEST(ActionClientStateMachineContract, ResultMayWinWhileCancelResponseIsPending)
{
    const auto transition = action::reduce_client_goal(
        client_context(action::GoalState::EXECUTING, true),
        {action::ClientGoalEventType::RESULT_RECEIVED,
            action::TerminalStatus::SUCCEEDED});
    EXPECT_EQ(transition.decision, TD::ALLOW);
    EXPECT_EQ(transition.next.state, action::GoalState::FINISHING);
    EXPECT_FALSE(transition.next.cancel_response_pending);
    EXPECT_TRUE(action::has_effect(
        transition.effects, TE::DELIVER_RESULT));
}

TEST(ActionClientStateMachineContract, RuntimeFailuresDoNotInventTerminalStatus)
{
    const auto send_failed = action::reduce_client_goal(
        client_context(action::GoalState::EXECUTING, true),
        {action::ClientGoalEventType::CANCEL_REQUEST_SEND_FAILED});
    EXPECT_EQ(send_failed.decision, TD::ALLOW);
    EXPECT_FALSE(send_failed.next.cancel_response_pending);
    EXPECT_EQ(
        send_failed.next.terminal_status,
        action::TerminalStatus::UNSPECIFIED);
    EXPECT_TRUE(action::has_effect(
        send_failed.effects, TE::NOTIFY_RUNTIME_ERROR));

    for (const auto event : {
             action::ClientGoalEventType::CANCEL_RESPONSE_TIMEOUT,
             action::ClientGoalEventType::RESULT_TIMEOUT,
             action::ClientGoalEventType::TRANSPORT_DISCONNECTED,
             action::ClientGoalEventType::CLIENT_SHUTDOWN_REQUESTED}) {
        const auto transition = action::reduce_client_goal(
            client_context(action::GoalState::EXECUTING), {event});
        EXPECT_EQ(transition.decision, TD::DEFER);
        EXPECT_EQ(
            transition.next.terminal_status,
            action::TerminalStatus::UNSPECIFIED);
    }
}

TEST(ActionClientStateMachineContract, InvalidPrimaryStateIsInvariantViolation)
{
    const auto transition = action::reduce_client_goal(
        client_context(action::GoalState::UNSPECIFIED),
        {action::ClientGoalEventType::REQUEST_CANCEL});
    EXPECT_EQ(transition.decision, TD::INVARIANT_VIOLATION);
}

TEST(ActionClientStateMachineContract, ImpossibleContextCombinationIsInvariantViolation)
{
    auto client = client_context(action::GoalState::CANCELING);
    client.cancel_response_pending = true;
    const auto transition = action::reduce_client_goal(
        client,
        {action::ClientGoalEventType::FEEDBACK_RECEIVED});
    EXPECT_EQ(transition.decision, TD::INVARIANT_VIOLATION);
}
