#include "hakoniwa/pdu/action/action_server_state_machine.hpp"

#include <gtest/gtest.h>

#include <array>

namespace action = hakoniwa::pdu::action;

namespace {

using TK = action::ServerTransitionKind;
using TE = action::ServerTransitionError;

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

TEST(ActionServerStateMachineContract, FeedbackIsNopWhenStateAllowsIt)
{
    for (const auto state : {action::GoalState::EXECUTING,
             action::GoalState::CANCELING}) {
        const auto transition = action::transition_server_goal(
            server_context(state),
            action::ServerGoalEvent::PUBLISH_FEEDBACK);
        EXPECT_TRUE(transition.is_nop());
        EXPECT_EQ(transition.next.state, state);
        EXPECT_EQ(transition.error, TE::NONE);
    }

    const auto finishing = action::transition_server_goal(
        server_context(action::GoalState::FINISHING),
        action::ServerGoalEvent::PUBLISH_FEEDBACK);
    EXPECT_TRUE(finishing.is_error());
    EXPECT_EQ(finishing.error, TE::EVENT_NOT_ALLOWED);
}

TEST(ActionServerStateMachineContract, TerminalCompletionFollowsServerStateMatrix)
{
    struct Case {
        action::GoalState state;
        action::ServerGoalEvent event;
        TK kind;
        action::GoalState next;
        action::TerminalStatus terminal;
        TE error;
    };
    constexpr std::array cases{
        Case{action::GoalState::EXECUTING,
            action::ServerGoalEvent::COMPLETE_SUCCEEDED,
            TK::TRANSITIONED,
            action::GoalState::FINISHING,
            action::TerminalStatus::SUCCEEDED,
            TE::NONE},
        Case{action::GoalState::EXECUTING,
            action::ServerGoalEvent::COMPLETE_CANCELED,
            TK::ERROR,
            action::GoalState::EXECUTING,
            action::TerminalStatus::UNSPECIFIED,
            TE::EVENT_NOT_ALLOWED},
        Case{action::GoalState::EXECUTING,
            action::ServerGoalEvent::COMPLETE_ABORTED,
            TK::TRANSITIONED,
            action::GoalState::FINISHING,
            action::TerminalStatus::ABORTED,
            TE::NONE},
        Case{action::GoalState::CANCELING,
            action::ServerGoalEvent::COMPLETE_SUCCEEDED,
            TK::ERROR,
            action::GoalState::CANCELING,
            action::TerminalStatus::UNSPECIFIED,
            TE::EVENT_NOT_ALLOWED},
        Case{action::GoalState::CANCELING,
            action::ServerGoalEvent::COMPLETE_CANCELED,
            TK::TRANSITIONED,
            action::GoalState::FINISHING,
            action::TerminalStatus::CANCELED,
            TE::NONE},
        Case{action::GoalState::CANCELING,
            action::ServerGoalEvent::COMPLETE_ABORTED,
            TK::TRANSITIONED,
            action::GoalState::FINISHING,
            action::TerminalStatus::ABORTED,
            TE::NONE},
    };

    for (const auto& test_case : cases) {
        const auto transition = action::transition_server_goal(
            server_context(test_case.state), test_case.event);
        EXPECT_EQ(transition.kind, test_case.kind);
        EXPECT_EQ(transition.next.state, test_case.next);
        EXPECT_EQ(transition.next.terminal_status, test_case.terminal);
        EXPECT_EQ(transition.error, test_case.error);
    }

    const auto unspecified = action::transition_server_goal(
        server_context(action::GoalState::EXECUTING),
        action::ServerGoalEvent::COMPLETE_UNSPECIFIED);
    EXPECT_TRUE(unspecified.is_error());
    EXPECT_EQ(unspecified.error, TE::INVALID_TERMINAL_STATUS);
}

TEST(ActionServerStateMachineContract, ClientCancelRequestAndDecisionTransitionContext)
{
    const auto requested = action::transition_server_goal(
        server_context(action::GoalState::EXECUTING),
        action::ServerGoalEvent::CANCEL_REQUEST_RECEIVED);
    ASSERT_TRUE(requested.transitioned());
    EXPECT_TRUE(requested.next.cancel_decision_pending);
    EXPECT_EQ(requested.next.cancel_origin, action::CancelOrigin::CLIENT);

    const auto accepted = action::transition_server_goal(
        requested.next, action::ServerGoalEvent::ACCEPT_CANCEL);
    EXPECT_TRUE(accepted.transitioned());
    EXPECT_EQ(accepted.next.state, action::GoalState::CANCELING);
    EXPECT_FALSE(accepted.next.cancel_decision_pending);

    const auto rejected = action::transition_server_goal(
        requested.next, action::ServerGoalEvent::REJECT_CANCEL);
    EXPECT_TRUE(rejected.transitioned());
    EXPECT_EQ(rejected.next.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(rejected.next.cancel_decision_pending);
}

TEST(ActionServerStateMachineContract, CancelDecisionRequiresPendingRequest)
{
    for (const auto event : {action::ServerGoalEvent::ACCEPT_CANCEL,
             action::ServerGoalEvent::REJECT_CANCEL}) {
        const auto transition = action::transition_server_goal(
            server_context(action::GoalState::EXECUTING), event);
        EXPECT_TRUE(transition.is_error());
        EXPECT_EQ(transition.error, TE::CANCEL_DECISION_NOT_PENDING);
    }
}

TEST(ActionServerStateMachineContract, RuntimeCancelUsesSameStateTransition)
{
    const auto requested = action::transition_server_goal(
        server_context(action::GoalState::EXECUTING),
        action::ServerGoalEvent::RUNTIME_CANCEL_REQUESTED);
    ASSERT_TRUE(requested.transitioned());
    EXPECT_EQ(requested.next.cancel_origin, action::CancelOrigin::RUNTIME);

    const auto accepted = action::transition_server_goal(
        requested.next, action::ServerGoalEvent::ACCEPT_CANCEL);
    EXPECT_TRUE(accepted.transitioned());
    EXPECT_EQ(accepted.next.state, action::GoalState::CANCELING);
}

TEST(ActionServerStateMachineContract, DisconnectReclassifiesPendingClientCancel)
{
    const auto client_requested = action::transition_server_goal(
        server_context(action::GoalState::EXECUTING),
        action::ServerGoalEvent::CANCEL_REQUEST_RECEIVED);
    ASSERT_TRUE(client_requested.transitioned());
    ASSERT_EQ(
        client_requested.next.cancel_origin,
        action::CancelOrigin::CLIENT);

    const auto disconnected = action::transition_server_goal(
        client_requested.next,
        action::ServerGoalEvent::RUNTIME_CANCEL_REQUESTED);
    ASSERT_TRUE(disconnected.transitioned());
    EXPECT_TRUE(disconnected.next.cancel_decision_pending);
    EXPECT_EQ(
        disconnected.next.cancel_origin,
        action::CancelOrigin::RUNTIME);
    EXPECT_EQ(disconnected.next.state, action::GoalState::EXECUTING);
}

TEST(ActionServerStateMachineContract, DuplicateAndLateEventsAreNop)
{
    for (const auto state : {action::GoalState::EXECUTING,
             action::GoalState::CANCELING,
             action::GoalState::FINISHING}) {
        const auto duplicate_goal = action::transition_server_goal(
            server_context(state),
            action::ServerGoalEvent::DUPLICATE_GOAL_REQUEST_RECEIVED);
        EXPECT_TRUE(duplicate_goal.is_nop());
        EXPECT_EQ(duplicate_goal.next.state, state);

        const auto duplicate_cancel = action::transition_server_goal(
            server_context(state),
            action::ServerGoalEvent::DUPLICATE_CANCEL_REQUEST_RECEIVED);
        EXPECT_TRUE(duplicate_cancel.is_nop());
        EXPECT_EQ(duplicate_cancel.next.state, state);
    }
}

TEST(ActionServerStateMachineContract, ResultDeliveryRequiresFinishingState)
{
    for (const auto state : {action::GoalState::EXECUTING,
             action::GoalState::CANCELING}) {
        const auto transition = action::transition_server_goal(
            server_context(state),
            action::ServerGoalEvent::RESULT_SEND_COMPLETED);
        EXPECT_TRUE(transition.is_error());
        EXPECT_EQ(transition.error, TE::EVENT_NOT_ALLOWED);
    }

    const auto completed = action::transition_server_goal(
        server_context(action::GoalState::FINISHING),
        action::ServerGoalEvent::RESULT_SEND_COMPLETED);
    EXPECT_TRUE(completed.is_nop());

    const auto failed = action::transition_server_goal(
        server_context(action::GoalState::FINISHING),
        action::ServerGoalEvent::RESULT_SEND_FAILED);
    EXPECT_TRUE(failed.is_nop());
}

TEST(ActionServerStateMachineContract, InvalidContextReturnsReason)
{
    auto invalid_state = server_context(action::GoalState::UNSPECIFIED);
    auto transition = action::transition_server_goal(
        invalid_state,
        action::ServerGoalEvent::PUBLISH_FEEDBACK);
    EXPECT_TRUE(transition.is_error());
    EXPECT_EQ(transition.error, TE::INVALID_CONTEXT);
    EXPECT_EQ(
        action::server_transition_error_name(transition.error),
        "invalid_context");

    auto impossible = server_context(action::GoalState::CANCELING);
    impossible.cancel_decision_pending = true;
    impossible.cancel_origin = action::CancelOrigin::CLIENT;
    transition = action::transition_server_goal(
        impossible,
        action::ServerGoalEvent::PUBLISH_FEEDBACK);
    EXPECT_TRUE(transition.is_error());
    EXPECT_EQ(transition.error, TE::INVALID_CONTEXT);
}
