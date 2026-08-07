#include "hakoniwa/pdu/action/action_services_client.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>

namespace action = hakoniwa::pdu::action;

namespace hakoniwa::pdu::action {

class ActionServicesClientTestPeer {
public:
    static void add_action(
        ActionServicesClient& client,
        std::string action_name,
        std::shared_ptr<IActionClientEndpoint> endpoint)
    {
        std::lock_guard<std::mutex> lock(client.mutex_);
        client.actions_.push_back(ActionServicesClient::ActionInstance{
            std::move(action_name),
            std::move(endpoint),
            {},
        });
    }

    static std::size_t goal_count(
        const ActionServicesClient& client,
        const std::string& action_name)
    {
        std::lock_guard<std::mutex> lock(client.mutex_);
        for (const auto& action : client.actions_) {
            if (action.action_name == action_name) {
                return action.goals.size();
            }
        }
        return 0;
    }

    static ClientGoalContext goal_context(
        const ActionServicesClient& client,
        const std::string& action_name,
        const GoalId& goal_id)
    {
        std::lock_guard<std::mutex> lock(client.mutex_);
        for (const auto& action : client.actions_) {
            if (action.action_name != action_name) {
                continue;
            }
            for (const auto& goal : action.goals) {
                if (goal.goal.goal_id == goal_id) {
                    return goal.context;
                }
            }
        }
        return ClientGoalContext{GoalState::UNSPECIFIED};
    }
};

} // namespace hakoniwa::pdu::action

namespace {

action::GoalId test_goal_id(std::uint8_t value = 1)
{
    action::GoalId goal_id{};
    goal_id[0] = value;
    return goal_id;
}

class FakeActionClientEndpoint final : public action::IActionClientEndpoint {
public:
    explicit FakeActionClientEndpoint(std::string action_name)
        : IActionClientEndpoint(
              std::move(action_name), "test-client", 1000)
    {
    }

    bool initialize(const nlohmann::json&) override { return true; }
    bool send_goal(
        const action::PduData&,
        const action::GoalId& goal_id,
        action::ClientGoalHandle& goal_handle_out,
        std::uint64_t timeout_usec) override
    {
        ++send_goal_calls;
        last_timeout_usec = timeout_usec;
        if (!send_goal_result) {
            goal_handle_out = action::ClientGoalHandle{};
            return false;
        }
        goal_handle_out.goal_id = goal_id;
        return true;
    }
    bool send_cancel(const action::ClientGoalHandle&) override
    {
        ++send_cancel_calls;
        return send_cancel_result;
    }
    action::ClientEventType poll(action::ClientEvent& event_out) override
    {
        if (events.empty()) {
            return action::ClientEventType::NONE;
        }
        event_out = std::move(events.front());
        events.pop_front();
        return event_out.type;
    }
    bool create_goal_buffer(action::PduData&) override { return true; }
    void clear_pending_events() override {}
    void reset_contexts() override {}

    void push_event(action::ClientEvent event)
    {
        events.push_back(std::move(event));
    }

    bool send_goal_result{true};
    bool send_cancel_result{true};
    int send_goal_calls{0};
    int send_cancel_calls{0};
    std::uint64_t last_timeout_usec{0};
    std::deque<action::ClientEvent> events;
};

action::ActionServicesClient client()
{
    return action::ActionServicesClient(
        "client-node",
        "test-client",
        "unused.json",
        "ActionClientEndpointImpl",
        1000);
}

action::ClientEvent goal_response(
    const action::GoalId& goal_id,
    action::Decision decision)
{
    action::ClientEvent event;
    event.type = action::ClientEventType::GOAL_RESPONSE;
    event.goal.goal_id = goal_id;
    event.decision = decision;
    return event;
}

} // namespace

TEST(ActionServicesClientGoalInstanceContract, SendGoalDoesNotCreateAcceptedInstance)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    action::ClientGoalHandle goal;

    EXPECT_TRUE(services.send_goal(
        "demo", {0x47}, test_goal_id(), goal, 5000));
    EXPECT_TRUE(goal.valid());
    EXPECT_EQ(endpoint->send_goal_calls, 1);
    EXPECT_EQ(endpoint->last_timeout_usec, 5000U);
    EXPECT_EQ(
        action::ActionServicesClientTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesClientGoalInstanceContract, AcceptedResponseCreatesExecutingInstance)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    const auto goal_id = test_goal_id();
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        goal_response(goal_id, action::Decision::ACCEPTED));

    std::string action_name;
    action::ClientEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(action_name, "demo");
    EXPECT_EQ(event.action_name, "demo");
    EXPECT_EQ(event.goal.goal_id, goal_id);
    EXPECT_EQ(
        action::ActionServicesClientTestPeer::goal_count(services, "demo"),
        1U);
    const auto context =
        action::ActionServicesClientTestPeer::goal_context(
            services, "demo", goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_TRUE(context.result_pending);
}

TEST(ActionServicesClientGoalInstanceContract, RejectedResponseCreatesNoInstance)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        goal_response(test_goal_id(), action::Decision::REJECTED));

    std::string action_name;
    action::ClientEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(
        action::ActionServicesClientTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesClientGoalInstanceContract, EndpointSendFailureCreatesNoInstance)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    endpoint->send_goal_result = false;
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    action::ClientGoalHandle goal;

    EXPECT_FALSE(services.send_goal(
        "demo", {0x47}, test_goal_id(), goal));
    EXPECT_FALSE(goal.valid());
    EXPECT_EQ(
        action::ActionServicesClientTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesClientGoalInstanceContract, DuplicateAcceptedResponseIsError)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    const auto goal_id = test_goal_id();
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        goal_response(goal_id, action::Decision::ACCEPTED));
    endpoint->push_event(
        goal_response(goal_id, action::Decision::ACCEPTED));

    std::string action_name;
    action::ClientEvent event;
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::GOAL_RESPONSE);
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::ERROR);
    EXPECT_EQ(
        action::ActionServicesClientTestPeer::goal_count(services, "demo"),
        1U);
}

TEST(ActionServicesClientGoalInstanceContract, UnknownActionCannotSendGoal)
{
    auto services = client();
    action::ClientGoalHandle goal;
    EXPECT_FALSE(services.send_goal(
        "missing", {0x47}, test_goal_id(), goal));
    EXPECT_FALSE(goal.valid());
}

TEST(ActionServicesClientGoalInstanceContract, CancelSendCommitsPendingAfterSuccess)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    const auto goal_id = test_goal_id();
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        goal_response(goal_id, action::Decision::ACCEPTED));
    std::string action_name;
    action::ClientEvent event;
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::GOAL_RESPONSE);

    EXPECT_TRUE(services.send_cancel("demo", event.goal));
    EXPECT_EQ(endpoint->send_cancel_calls, 1);
    const auto context =
        action::ActionServicesClientTestPeer::goal_context(
            services, "demo", goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_TRUE(context.cancel_response_pending);
}

TEST(ActionServicesClientGoalInstanceContract, CancelSendFailureKeepsGoalRetryable)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    endpoint->send_cancel_result = false;
    const auto goal_id = test_goal_id();
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        goal_response(goal_id, action::Decision::ACCEPTED));
    std::string action_name;
    action::ClientEvent event;
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::GOAL_RESPONSE);

    EXPECT_FALSE(services.send_cancel("demo", event.goal));
    auto context = action::ActionServicesClientTestPeer::goal_context(
        services, "demo", goal_id);
    EXPECT_FALSE(context.cancel_response_pending);

    endpoint->send_cancel_result = true;
    EXPECT_TRUE(services.send_cancel("demo", event.goal));
    context = action::ActionServicesClientTestPeer::goal_context(
        services, "demo", goal_id);
    EXPECT_TRUE(context.cancel_response_pending);
    EXPECT_EQ(endpoint->send_cancel_calls, 2);
}

TEST(ActionServicesClientGoalInstanceContract, DuplicateCancelIsRejectedBeforeEndpoint)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    const auto goal_id = test_goal_id();
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(
        goal_response(goal_id, action::Decision::ACCEPTED));
    std::string action_name;
    action::ClientEvent event;
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ClientEventType::GOAL_RESPONSE);

    ASSERT_TRUE(services.send_cancel("demo", event.goal));
    EXPECT_FALSE(services.send_cancel("demo", event.goal));
    EXPECT_EQ(endpoint->send_cancel_calls, 1);
}

TEST(ActionServicesClientGoalInstanceContract, UnknownAcceptedGoalCannotCancel)
{
    auto services = client();
    auto endpoint = std::make_shared<FakeActionClientEndpoint>("demo");
    action::ActionServicesClientTestPeer::add_action(
        services, "demo", endpoint);

    EXPECT_FALSE(services.send_cancel(
        "demo", action::ClientGoalHandle{test_goal_id()}));
    EXPECT_EQ(endpoint->send_cancel_calls, 0);
}
