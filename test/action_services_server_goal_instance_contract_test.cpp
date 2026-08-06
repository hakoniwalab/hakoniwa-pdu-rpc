#include "hakoniwa/pdu/action/action_services_server.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>

namespace action = hakoniwa::pdu::action;

namespace hakoniwa::pdu::action {

class ActionServicesServerTestPeer {
public:
    static void add_action(
        ActionServicesServer& server,
        std::string action_name,
        std::shared_ptr<IActionServerEndpoint> endpoint)
    {
        std::lock_guard<std::mutex> lock(server.mutex_);
        server.actions_.push_back(ActionServicesServer::ActionInstance{
            std::move(action_name),
            std::move(endpoint),
            {},
        });
    }

    static std::size_t goal_count(
        const ActionServicesServer& server,
        const std::string& action_name)
    {
        std::lock_guard<std::mutex> lock(server.mutex_);
        for (const auto& action : server.actions_) {
            if (action.action_name == action_name) {
                return action.goals.size();
            }
        }
        return 0;
    }

    static ServerGoalContext goal_context(
        const ActionServicesServer& server,
        const std::string& action_name,
        const GoalId& goal_id)
    {
        std::lock_guard<std::mutex> lock(server.mutex_);
        for (const auto& action : server.actions_) {
            if (action.action_name != action_name) {
                continue;
            }
            for (const auto& goal : action.goals) {
                if (goal.goal.goal_id == goal_id) {
                    return goal.context;
                }
            }
        }
        return ServerGoalContext{GoalState::UNSPECIFIED};
    }
};

} // namespace hakoniwa::pdu::action

namespace {

action::ServerGoalHandle test_goal(std::uint8_t value = 1)
{
    action::ServerGoalHandle goal{};
    goal.goal_id[0] = value;
    return goal;
}

class FakeActionServerEndpoint final : public action::IActionServerEndpoint {
public:
    explicit FakeActionServerEndpoint(std::string action_name)
        : IActionServerEndpoint(std::move(action_name), 1000)
    {
    }

    bool initialize(const nlohmann::json&) override { return true; }
    action::ServerEventType poll(action::ServerEvent& event_out) override
    {
        if (events.empty()) {
            return action::ServerEventType::NONE;
        }
        event_out = std::move(events.front());
        events.pop_front();
        return event_out.type;
    }
    bool accept_goal(const action::ServerGoalHandle&) override
    {
        ++accept_calls;
        return accept_result;
    }
    bool reject_goal(const action::ServerGoalHandle&) override { return true; }
    bool accept_cancel(const action::ServerGoalHandle&) override { return true; }
    bool reject_cancel(const action::ServerGoalHandle&) override { return true; }
    bool create_result_buffer(action::PduData&) override { return true; }
    bool create_feedback_buffer(action::PduData&) override { return true; }
    bool send_feedback(
        const action::ServerGoalHandle&,
        const action::PduData&) override
    {
        return true;
    }
    bool complete(
        const action::ServerGoalHandle&,
        action::TerminalStatus,
        const action::PduData&) override
    {
        return true;
    }
    void clear_pending_events() override {}
    void reset_contexts() override {}

    void push_event(action::ServerEvent event)
    {
        events.push_back(std::move(event));
    }

    bool accept_result{true};
    int accept_calls{0};
    std::deque<action::ServerEvent> events;
};

action::ActionServicesServer server()
{
    return action::ActionServicesServer(
        "server-node", "unused.json", "ActionServerEndpointImpl", 1000);
}

action::ServerEvent server_event(
    action::ServerEventType type,
    const action::ServerGoalHandle& goal)
{
    action::ServerEvent event;
    event.type = type;
    event.goal = goal;
    return event;
}

} // namespace

TEST(ActionServicesServerGoalInstanceContract, UnknownActionIsRejected)
{
    auto services = server();
    EXPECT_FALSE(services.accept_goal("missing", test_goal()));
}

TEST(ActionServicesServerGoalInstanceContract, EndpointFailureDoesNotAddGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->accept_result = false;
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    EXPECT_FALSE(services.accept_goal("demo", test_goal()));
    EXPECT_EQ(endpoint->accept_calls, 1);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, SuccessfulAcceptAddsExecutingGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    ASSERT_TRUE(services.accept_goal("demo", goal));
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        1U);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(context.cancel_decision_pending);
    EXPECT_EQ(context.terminal_status, action::TerminalStatus::UNSPECIFIED);
}

TEST(ActionServicesServerGoalInstanceContract, DuplicateGoalIsRejectedBeforeEndpoint)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    ASSERT_TRUE(services.accept_goal("demo", goal));
    EXPECT_FALSE(services.accept_goal("demo", goal));
    EXPECT_EQ(endpoint->accept_calls, 1);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        1U);
}

TEST(ActionServicesServerGoalInstanceContract, PollPassesGoalRequestWithoutCreatingInstance)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        server_event(action::ServerEventType::GOAL_REQUEST, goal));

    std::string action_name;
    action::ServerEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::GOAL_REQUEST);
    EXPECT_EQ(action_name, "demo");
    EXPECT_EQ(event.action_name, "demo");
    EXPECT_EQ(event.goal.goal_id, goal.goal_id);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, PollClientCancelUpdatesGoalContext)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));
    endpoint->push_event(
        server_event(action::ServerEventType::CANCEL_REQUEST, goal));

    std::string action_name;
    action::ServerEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::CANCEL_REQUEST);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_TRUE(context.cancel_decision_pending);
    EXPECT_EQ(context.cancel_origin, action::CancelOrigin::CLIENT);
}

TEST(ActionServicesServerGoalInstanceContract, PollRuntimeCancelUsesSameGoalContext)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));
    endpoint->push_event(
        server_event(action::ServerEventType::RUNTIME_CANCEL_REQUEST, goal));

    std::string action_name;
    action::ServerEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_TRUE(context.cancel_decision_pending);
    EXPECT_EQ(context.cancel_origin, action::CancelOrigin::RUNTIME);
}

TEST(ActionServicesServerGoalInstanceContract, PollCancelForUnknownGoalReturnsError)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(server_event(
        action::ServerEventType::CANCEL_REQUEST, test_goal(9)));

    std::string action_name;
    action::ServerEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::ERROR);
    EXPECT_EQ(action_name, "demo");
    EXPECT_EQ(event.type, action::ServerEventType::ERROR);
}
