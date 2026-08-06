#include "hakoniwa/pdu/action/action_server_state_machine.hpp"

#include <gtest/gtest.h>

#include <array>

namespace action = hakoniwa::pdu::action;

namespace {

using TD = action::TransitionDecision;
using TE = action::TransitionEffect;

action::ServerGoalContext server_context(
    action::GoalState state,
    bool cancel_pending = false,
    action::CancelOrigin origin = action::CancelOrigin::NONE)
{
    return action::ServerGoalContext{
        state,
        cancel_pending,
        origin,
        state == action::GoalState::FINISHING
            ? action::TerminalStatus::SUCCEEDED
            : action::TerminalStatus::UNSPECIFIED,
    };
}

} // namespace

TEST(ActionServerStateMachineContract, FeedbackFollowsServerStateMatrix)
{
    for (const auto state : {action::GoalState::EXECUTING,
             action::GoalState::CANCELING}) {
        const auto transition = action::reduce_server_goal(
            server_context(state),
            action::ServerGoalEvent::PUBLISH_FEEDBACK);
        EXPECT_EQ(transition.decision, TD::ALLOW);
        EXPECT_EQ(transition.next.state, state);
        EXPECT_TRUE(action::has_effect(
            transition.effects, TE::PUBLISH_FEEDBACK));
    }

    const auto finishing = action::reduce_server_goal(
        server_context(action::GoalState::FINISHING),
        action::ServerGoalEvent::PUBLISH_FEEDBACK);
    EXPECT_EQ(finishing.decision, TD::APPLICATION_API_ERROR);
    EXPECT_EQ(finishing.next.state, action::GoalState::FINISHING);
}

TEST(ActionServerStateMachineContract, TerminalCompletionFollowsServerStateMatrix)
{
    struct Case {
        action::GoalState state;
        action::ServerGoalEvent event;
        TD decision;
        action::GoalState next;
        action::TerminalStatus terminal;
    };
    constexpr std::array cases{
        Case{action::GoalState::EXECUTING,
            action::ServerGoalEvent::COMPLETE_SUCCEEDED,
            TD::ALLOW,
            action::GoalState::FINISHING,
            action::TerminalStatus::SUCCEEDED},
        Case{action::GoalState::EXECUTING,
            action::ServerGoalEvent::COMPLETE_CANCELED,
            TD::APPLICATION_API_ERROR,
            action::GoalState::EXECUTING,
            action::TerminalStatus::UNSPECIFIED},
        Case{action::GoalState::EXECUTING,
            action::ServerGoalEvent::COMPLETE_ABORTED,
            TD::ALLOW,
            action::GoalState::FINISHING,
            action::TerminalStatus::ABORTED},
        Case{action::GoalState::CANCELING,
            action::ServerGoalEvent::COMPLETE_SUCCEEDED,
            TD::APPLICATION_API_ERROR,
            action::GoalState::CANCELING,
            action::TerminalStatus::UNSPECIFIED},
        Case{action::GoalState::CANCELING,
            action::ServerGoalEvent::COMPLETE_CANCELED,
            TD::ALLOW,
            action::GoalState::FINISHING,
            action::TerminalStatus::CANCELED},
        Case{action::GoalState::CANCELING,
            action::ServerGoalEvent::COMPLETE_ABORTED,
            TD::ALLOW,
            action::GoalState::FINISHING,
            action::TerminalStatus::ABORTED},
    };

    for (const auto& test_case : cases) {
        const auto transition = action::reduce_server_goal(
            server_context(test_case.state), test_case.event);
        EXPECT_EQ(transition.decision, test_case.decision);
        EXPECT_EQ(transition.next.state, test_case.next);
        EXPECT_EQ(transition.next.terminal_status, test_case.terminal);
        EXPECT_EQ(
            action::has_effect(transition.effects, TE::COMMIT_RESULT),
            test_case.decision == TD::ALLOW);
        if (test_case.decision == TD::ALLOW) {
            EXPECT_EQ(
                transition.commit,
                action::TransitionCommit::IMMEDIATE);
        }
    }

    for (const auto event : {
             action::ServerGoalEvent::COMPLETE_SUCCEEDED,
             action::ServerGoalEvent::COMPLETE_CANCELED,
             action::ServerGoalEvent::COMPLETE_ABORTED}) {
        const auto transition = action::reduce_server_goal(
            server_context(action::GoalState::FINISHING), event);
        EXPECT_EQ(transition.decision, TD::APPLICATION_API_ERROR);
        EXPECT_EQ(transition.next.state, action::GoalState::FINISHING);
    }
}

TEST(ActionServerStateMachineContract, ClientCancelDecisionHasExplicitOrigin)
{
    const auto requested = action::reduce_server_goal(
        server_context(action::GoalState::EXECUTING),
        action::ServerGoalEvent::CANCEL_REQUEST_RECEIVED);
    ASSERT_EQ(requested.decision, TD::DEFER);
    EXPECT_TRUE(requested.next.cancel_decision_pending);
    EXPECT_EQ(requested.next.cancel_origin, action::CancelOrigin::CLIENT);
    EXPECT_TRUE(action::has_effect(
        requested.effects, TE::NOTIFY_CANCEL_REQUEST));

    const auto accepted = action::reduce_server_goal(
        requested.next, action::ServerGoalEvent::ACCEPT_CANCEL);
    EXPECT_EQ(accepted.decision, TD::ALLOW);
    EXPECT_EQ(accepted.next.state, action::GoalState::CANCELING);
    EXPECT_FALSE(accepted.next.cancel_decision_pending);
    EXPECT_TRUE(action::has_effect(
        accepted.effects, TE::SEND_CANCEL_RESPONSE_ACCEPTED));
    EXPECT_EQ(
        accepted.commit,
        action::TransitionCommit::AFTER_EFFECT_SUCCESS);

    const auto rejected = action::reduce_server_goal(
        requested.next, action::ServerGoalEvent::REJECT_CANCEL);
    EXPECT_EQ(rejected.decision, TD::ALLOW);
    EXPECT_EQ(rejected.next.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(rejected.next.cancel_decision_pending);
    EXPECT_TRUE(action::has_effect(
        rejected.effects, TE::SEND_CANCEL_RESPONSE_REJECTED));
    EXPECT_EQ(
        rejected.commit,
        action::TransitionCommit::AFTER_EFFECT_SUCCESS);
}

TEST(ActionServerStateMachineContract, RuntimeCancelUsesSameDecisionPathWithoutWireResponse)
{
    const auto requested = action::reduce_server_goal(
        server_context(action::GoalState::EXECUTING),
        action::ServerGoalEvent::RUNTIME_CANCEL_REQUESTED);
    ASSERT_EQ(requested.decision, TD::DEFER);
    EXPECT_EQ(requested.next.cancel_origin, action::CancelOrigin::RUNTIME);

    const auto accepted = action::reduce_server_goal(
        requested.next, action::ServerGoalEvent::ACCEPT_CANCEL);
    EXPECT_EQ(accepted.decision, TD::ALLOW);
    EXPECT_EQ(accepted.next.state, action::GoalState::CANCELING);
    EXPECT_FALSE(action::has_effect(
        accepted.effects, TE::SEND_CANCEL_RESPONSE_ACCEPTED));
    EXPECT_EQ(accepted.commit, action::TransitionCommit::IMMEDIATE);
}

TEST(ActionServerStateMachineContract, DuplicateAndLateEventsCannotChangeCommittedState)
{
    for (const auto state : {action::GoalState::EXECUTING,
             action::GoalState::CANCELING,
             action::GoalState::FINISHING}) {
        const auto duplicate_goal = action::reduce_server_goal(
            server_context(state),
            action::ServerGoalEvent::DUPLICATE_GOAL_REQUEST_RECEIVED);
        EXPECT_EQ(duplicate_goal.decision, TD::PROTOCOL_REJECT);
        EXPECT_EQ(duplicate_goal.next.state, state);
        EXPECT_TRUE(action::has_effect(
            duplicate_goal.effects, TE::SEND_PROTOCOL_REJECT));

        const auto duplicate_cancel = action::reduce_server_goal(
            server_context(state),
            action::ServerGoalEvent::DUPLICATE_CANCEL_REQUEST_RECEIVED);
        EXPECT_EQ(duplicate_cancel.decision, TD::IGNORE);
        EXPECT_EQ(duplicate_cancel.next.state, state);
    }
}

TEST(ActionServerStateMachineContract, ResultDeliveryOnlyReleasesFinishingGoal)
{
    for (const auto state : {action::GoalState::EXECUTING,
             action::GoalState::CANCELING}) {
        const auto transition = action::reduce_server_goal(
            server_context(state),
            action::ServerGoalEvent::RESULT_SEND_COMPLETED);
        EXPECT_EQ(transition.decision, TD::INVARIANT_VIOLATION);
        EXPECT_FALSE(action::has_effect(
            transition.effects, TE::RELEASE_GOAL));
    }

    const auto completed = action::reduce_server_goal(
        server_context(action::GoalState::FINISHING),
        action::ServerGoalEvent::RESULT_SEND_COMPLETED);
    EXPECT_EQ(completed.decision, TD::ALLOW);
    EXPECT_TRUE(action::has_effect(completed.effects, TE::RELEASE_GOAL));

    const auto failed = action::reduce_server_goal(
        server_context(action::GoalState::FINISHING),
        action::ServerGoalEvent::RESULT_SEND_FAILED);
    EXPECT_EQ(failed.decision, TD::DEFER);
    EXPECT_TRUE(action::has_effect(
        failed.effects, TE::DEFER_TO_POLICY));
}

TEST(ActionServerStateMachineContract, InvalidPrimaryStateIsInvariantViolation)
{
    auto server = server_context(action::GoalState::UNSPECIFIED);
    EXPECT_EQ(
        action::reduce_server_goal(
            server, action::ServerGoalEvent::PUBLISH_FEEDBACK)
            .decision,
        TD::INVARIANT_VIOLATION);
}

TEST(ActionServerStateMachineContract, ImpossibleContextCombinationIsInvariantViolation)
{
    auto server = server_context(action::GoalState::CANCELING);
    server.cancel_decision_pending = true;
    server.cancel_origin = action::CancelOrigin::CLIENT;
    EXPECT_EQ(
        action::reduce_server_goal(
            server, action::ServerGoalEvent::PUBLISH_FEEDBACK)
            .decision,
        TD::INVARIANT_VIOLATION);
}
