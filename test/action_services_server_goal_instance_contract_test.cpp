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

    static bool set_goal_context(
        ActionServicesServer& server,
        const std::string& action_name,
        const GoalId& goal_id,
        const ServerGoalContext& context)
    {
        std::lock_guard<std::mutex> lock(server.mutex_);
        for (auto& action : server.actions_) {
            if (action.action_name != action_name) {
                continue;
            }
            for (auto& goal : action.goals) {
                if (goal.goal.goal_id == goal_id) {
                    goal.context = context;
                    return true;
                }
            }
        }
        return false;
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
    bool reject_goal(const action::ServerGoalHandle&) override
    {
        ++reject_calls;
        return reject_result;
    }
    bool accept_cancel(const action::ServerGoalHandle&) override
    {
        ++accept_cancel_calls;
        return accept_cancel_result;
    }
    bool reject_cancel(const action::ServerGoalHandle&) override
    {
        ++reject_cancel_calls;
        return reject_cancel_result;
    }
    bool create_result_buffer(action::PduData& pdu_out) override
    {
        ++create_result_calls;
        if (create_result_result) {
            pdu_out = {0x52};
        }
        return create_result_result;
    }
    bool create_feedback_buffer(action::PduData& pdu_out) override
    {
        ++create_feedback_calls;
        if (create_feedback_result) {
            pdu_out = {0x46};
        }
        return create_feedback_result;
    }
    bool send_feedback(
        const action::ServerGoalHandle&,
        const action::PduData&) override
    {
        ++send_feedback_calls;
        return send_feedback_result;
    }
    action::CompleteResult complete(
        const action::ServerGoalHandle&,
        action::TerminalStatus status,
        const action::PduData&) override
    {
        ++complete_calls;
        last_complete_status = status;
        return complete_result;
    }
    void clear_pending_events() override
    {
        ++clear_pending_events_calls;
        events.clear();
    }
    void reset_contexts() override
    {
        ++reset_contexts_calls;
        events.clear();
    }

    void push_event(action::ServerEvent event)
    {
        events.push_back(std::move(event));
    }

    bool accept_result{true};
    bool reject_result{true};
    bool create_feedback_result{true};
    bool create_result_result{true};
    bool accept_cancel_result{true};
    bool reject_cancel_result{true};
    bool send_feedback_result{true};
    action::CompleteResult complete_result{action::CompleteResult::SENT};
    int accept_calls{0};
    int reject_calls{0};
    int create_feedback_calls{0};
    int create_result_calls{0};
    int accept_cancel_calls{0};
    int reject_cancel_calls{0};
    int send_feedback_calls{0};
    int complete_calls{0};
    int clear_pending_events_calls{0};
    int reset_contexts_calls{0};
    action::TerminalStatus last_complete_status{
        action::TerminalStatus::UNSPECIFIED};
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

TEST(ActionServicesServerGoalInstanceContract, StopClearsPendingEndpointEvents)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    endpoint->push_event(server_event(
        action::ServerEventType::GOAL_REQUEST, test_goal()));

    services.stop_all_services();
    EXPECT_EQ(endpoint->clear_pending_events_calls, 1);
    std::string action_name;
    action::ServerEvent event;
    EXPECT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::NONE);
}

TEST(ActionServicesServerGoalInstanceContract, ClearResetsGoalsAndEndpointContexts)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    services.clear_all_instances();
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
    EXPECT_EQ(endpoint->reset_contexts_calls, 1);
}

TEST(ActionServicesServerGoalInstanceContract, InitializesConfiguredServerEndpoint)
{
    auto container = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        "fibonacci-server", ACTION_SERVER_CONTAINER_FIXTURE_PATH);
    ASSERT_EQ(container->initialize(), HAKO_PDU_ERR_OK);
    action::ActionServicesServer services(
        "fibonacci-server",
        ACTION_CONFIG_FIXTURE_PATH,
        "ActionServerEndpointImpl",
        1000,
        "virtual");

    ASSERT_TRUE(services.initialize_services(container));
    EXPECT_TRUE(services.start_all_services());
    action::PduData feedback;
    action::PduData result;
    EXPECT_TRUE(services.create_feedback_buffer("fibonacci", feedback));
    EXPECT_TRUE(services.create_result_buffer("fibonacci", result));
    EXPECT_FALSE(feedback.empty());
    EXPECT_FALSE(result.empty());
    EXPECT_FALSE(services.initialize_services(container));

    services.stop_all_services();
    services.clear_all_instances();
}

TEST(ActionServicesServerGoalInstanceContract, InitializesDirectMuxEndpoint)
{
    auto endpoint = std::make_shared<hakoniwa::pdu::Endpoint>(
        "accepted-connection", HAKO_PDU_ENDPOINT_DIRECTION_INOUT);
    ASSERT_EQ(
        endpoint->open(ACTION_SERVER_ENDPOINT_FIXTURE_PATH),
        HAKO_PDU_ERR_OK);
    action::ActionServicesServer services(
        "fibonacci-server",
        ACTION_CONFIG_FIXTURE_PATH,
        "ActionServerEndpointImpl",
        1000,
        "virtual");

    ASSERT_TRUE(services.initialize_services(endpoint));
    action::PduData result;
    EXPECT_TRUE(services.create_result_buffer("fibonacci", result));
    EXPECT_FALSE(result.empty());

    services.stop_all_services();
    services.clear_all_instances();
    EXPECT_EQ(endpoint->close(), HAKO_PDU_ERR_OK);
}

TEST(ActionServicesServerGoalInstanceContract, InitializationRequiresTransportOwner)
{
    action::ActionServicesServer services(
        "fibonacci-server",
        ACTION_CONFIG_FIXTURE_PATH,
        "ActionServerEndpointImpl",
        1000,
        "virtual");

    EXPECT_FALSE(services.initialize_services(
        std::shared_ptr<hakoniwa::pdu::EndpointContainer>{}));
    EXPECT_FALSE(services.initialize_services(
        std::shared_ptr<hakoniwa::pdu::Endpoint>{}));
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

TEST(ActionServicesServerGoalInstanceContract, SuccessfulRejectDoesNotAddGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    EXPECT_TRUE(services.reject_goal("demo", test_goal()));
    EXPECT_EQ(endpoint->reject_calls, 1);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, EndpointRejectFailureDoesNotAddGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->reject_result = false;
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    EXPECT_FALSE(services.reject_goal("demo", test_goal()));
    EXPECT_EQ(endpoint->reject_calls, 1);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, AcceptedGoalCannotBeRejected)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    ASSERT_TRUE(services.accept_goal("demo", goal));
    EXPECT_FALSE(services.reject_goal("demo", goal));
    EXPECT_EQ(endpoint->reject_calls, 0);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        1U);
}

TEST(ActionServicesServerGoalInstanceContract, CreatesBuffersThroughNamedActionEndpoint)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    action::PduData feedback;
    action::PduData result;
    EXPECT_TRUE(services.create_feedback_buffer("demo", feedback));
    EXPECT_TRUE(services.create_result_buffer("demo", result));
    EXPECT_EQ(feedback, action::PduData({0x46}));
    EXPECT_EQ(result, action::PduData({0x52}));
    EXPECT_EQ(endpoint->create_feedback_calls, 1);
    EXPECT_EQ(endpoint->create_result_calls, 1);
}

TEST(ActionServicesServerGoalInstanceContract, UnknownActionDoesNotCreateBuffers)
{
    auto services = server();
    action::PduData feedback;
    action::PduData result;

    EXPECT_FALSE(services.create_feedback_buffer("missing", feedback));
    EXPECT_FALSE(services.create_result_buffer("missing", result));
    EXPECT_TRUE(feedback.empty());
    EXPECT_TRUE(result.empty());
}

TEST(ActionServicesServerGoalInstanceContract, PropagatesBufferCreationFailure)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->create_feedback_result = false;
    endpoint->create_result_result = false;
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    action::PduData feedback;
    action::PduData result;
    EXPECT_FALSE(services.create_feedback_buffer("demo", feedback));
    EXPECT_FALSE(services.create_result_buffer("demo", result));
    EXPECT_EQ(endpoint->create_feedback_calls, 1);
    EXPECT_EQ(endpoint->create_result_calls, 1);
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

TEST(ActionServicesServerGoalInstanceContract, AcceptsClientCancelAfterResponseSend)
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
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::CANCEL_REQUEST);

    EXPECT_TRUE(services.accept_cancel("demo", goal));
    EXPECT_EQ(endpoint->accept_cancel_calls, 1);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::CANCELING);
    EXPECT_FALSE(context.cancel_decision_pending);
    EXPECT_EQ(context.cancel_origin, action::CancelOrigin::NONE);
}

TEST(ActionServicesServerGoalInstanceContract, CancelResponseFailureKeepsPendingDecision)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->accept_cancel_result = false;
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));
    endpoint->push_event(
        server_event(action::ServerEventType::CANCEL_REQUEST, goal));
    std::string action_name;
    action::ServerEvent event;
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::CANCEL_REQUEST);

    EXPECT_FALSE(services.accept_cancel("demo", goal));
    auto context = action::ActionServicesServerTestPeer::goal_context(
        services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_TRUE(context.cancel_decision_pending);
    EXPECT_EQ(context.cancel_origin, action::CancelOrigin::CLIENT);

    endpoint->accept_cancel_result = true;
    EXPECT_TRUE(services.accept_cancel("demo", goal));
    context = action::ActionServicesServerTestPeer::goal_context(
        services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::CANCELING);
    EXPECT_EQ(endpoint->accept_cancel_calls, 2);
}

TEST(ActionServicesServerGoalInstanceContract, RuntimeCancelAcceptanceNeedsNoWireResponse)
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
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);

    EXPECT_TRUE(services.accept_cancel("demo", goal));
    EXPECT_EQ(endpoint->accept_cancel_calls, 0);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::CANCELING);
    EXPECT_FALSE(context.cancel_decision_pending);
}

TEST(ActionServicesServerGoalInstanceContract, CannotAcceptCancelWithoutPendingRequest)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_FALSE(services.accept_cancel("demo", goal));
    EXPECT_EQ(endpoint->accept_cancel_calls, 0);
}

TEST(ActionServicesServerGoalInstanceContract, RejectsClientCancelAfterResponseSend)
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
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::CANCEL_REQUEST);

    EXPECT_TRUE(services.reject_cancel("demo", goal));
    EXPECT_EQ(endpoint->reject_cancel_calls, 1);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(context.cancel_decision_pending);
    EXPECT_EQ(context.cancel_origin, action::CancelOrigin::NONE);
}

TEST(ActionServicesServerGoalInstanceContract, RejectResponseFailureKeepsPendingDecision)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->reject_cancel_result = false;
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));
    endpoint->push_event(
        server_event(action::ServerEventType::CANCEL_REQUEST, goal));
    std::string action_name;
    action::ServerEvent event;
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::CANCEL_REQUEST);

    EXPECT_FALSE(services.reject_cancel("demo", goal));
    auto context = action::ActionServicesServerTestPeer::goal_context(
        services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_TRUE(context.cancel_decision_pending);
    EXPECT_EQ(context.cancel_origin, action::CancelOrigin::CLIENT);

    endpoint->reject_cancel_result = true;
    EXPECT_TRUE(services.reject_cancel("demo", goal));
    context = action::ActionServicesServerTestPeer::goal_context(
        services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(context.cancel_decision_pending);
    EXPECT_EQ(endpoint->reject_cancel_calls, 2);
}

TEST(ActionServicesServerGoalInstanceContract, RuntimeCancelRejectionNeedsNoWireResponse)
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
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);

    EXPECT_TRUE(services.reject_cancel("demo", goal));
    EXPECT_EQ(endpoint->reject_cancel_calls, 0);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
    EXPECT_FALSE(context.cancel_decision_pending);
}

TEST(ActionServicesServerGoalInstanceContract, CannotRejectCancelWithoutPendingRequest)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_FALSE(services.reject_cancel("demo", goal));
    EXPECT_EQ(endpoint->reject_cancel_calls, 0);
}

TEST(ActionServicesServerGoalInstanceContract, SendsFeedbackWhileExecuting)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_TRUE(services.send_feedback("demo", goal, {0x46}));
    EXPECT_EQ(endpoint->send_feedback_calls, 1);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
}

TEST(ActionServicesServerGoalInstanceContract, SendsFeedbackWhileCanceling)
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
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);
    ASSERT_TRUE(services.accept_cancel("demo", goal));

    EXPECT_TRUE(services.send_feedback("demo", goal, {0x46}));
    EXPECT_EQ(endpoint->send_feedback_calls, 1);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::CANCELING);
}

TEST(ActionServicesServerGoalInstanceContract, FeedbackSendFailureKeepsGoalState)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->send_feedback_result = false;
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_FALSE(services.send_feedback("demo", goal, {0x46}));
    EXPECT_EQ(endpoint->send_feedback_calls, 1);
    const auto context =
        action::ActionServicesServerTestPeer::goal_context(
            services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);
}

TEST(ActionServicesServerGoalInstanceContract, UnknownGoalCannotSendFeedback)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);

    EXPECT_FALSE(services.send_feedback("demo", test_goal(), {0x46}));
    EXPECT_EQ(endpoint->send_feedback_calls, 0);
}

TEST(ActionServicesServerGoalInstanceContract, FinishingGoalCannotSendFeedback)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));
    ASSERT_TRUE(action::ActionServicesServerTestPeer::set_goal_context(
        services,
        "demo",
        goal.goal_id,
        action::ServerGoalContext{
            action::GoalState::FINISHING,
            false,
            action::CancelOrigin::NONE,
            action::TerminalStatus::SUCCEEDED,
        }));

    EXPECT_FALSE(services.send_feedback("demo", goal, {0x46}));
    EXPECT_EQ(endpoint->send_feedback_calls, 0);
}

TEST(ActionServicesServerGoalInstanceContract, SuccessfulResultRemovesExecutingGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_TRUE(services.complete(
        "demo", goal, action::TerminalStatus::SUCCEEDED, {0x52}));
    EXPECT_EQ(endpoint->complete_calls, 1);
    EXPECT_EQ(
        endpoint->last_complete_status,
        action::TerminalStatus::SUCCEEDED);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, CanceledResultRequiresCancelingGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_FALSE(services.complete(
        "demo", goal, action::TerminalStatus::CANCELED, {0x52}));
    EXPECT_EQ(endpoint->complete_calls, 0);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        1U);
}

TEST(ActionServicesServerGoalInstanceContract, SuccessfulCanceledResultRemovesCancelingGoal)
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
    ASSERT_EQ(
        services.poll(action_name, event),
        action::ServerEventType::RUNTIME_CANCEL_REQUEST);
    ASSERT_TRUE(services.accept_cancel("demo", goal));

    EXPECT_TRUE(services.complete(
        "demo", goal, action::TerminalStatus::CANCELED, {0x52}));
    EXPECT_EQ(endpoint->complete_calls, 1);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, NotCommittedResultKeepsGoalRetryable)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->complete_result = action::CompleteResult::NOT_COMMITTED;
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_FALSE(services.complete(
        "demo", goal, action::TerminalStatus::SUCCEEDED, {0x52}));
    auto context = action::ActionServicesServerTestPeer::goal_context(
        services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::EXECUTING);

    endpoint->complete_result = action::CompleteResult::SENT;
    EXPECT_TRUE(services.complete(
        "demo", goal, action::TerminalStatus::SUCCEEDED, {0x52}));
    EXPECT_EQ(endpoint->complete_calls, 2);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        0U);
}

TEST(ActionServicesServerGoalInstanceContract, SendFailureAfterCommitKeepsFinishingGoal)
{
    auto services = server();
    auto endpoint = std::make_shared<FakeActionServerEndpoint>("demo");
    endpoint->complete_result =
        action::CompleteResult::SEND_FAILED_AFTER_COMMIT;
    const auto goal = test_goal();
    action::ActionServicesServerTestPeer::add_action(
        services, "demo", endpoint);
    ASSERT_TRUE(services.accept_goal("demo", goal));

    EXPECT_FALSE(services.complete(
        "demo", goal, action::TerminalStatus::SUCCEEDED, {0x52}));
    auto context = action::ActionServicesServerTestPeer::goal_context(
        services, "demo", goal.goal_id);
    EXPECT_EQ(context.state, action::GoalState::FINISHING);
    EXPECT_EQ(
        context.terminal_status,
        action::TerminalStatus::SUCCEEDED);
    EXPECT_EQ(
        action::ActionServicesServerTestPeer::goal_count(services, "demo"),
        1U);

    EXPECT_FALSE(services.complete(
        "demo", goal, action::TerminalStatus::SUCCEEDED, {0x52}));
    EXPECT_EQ(endpoint->complete_calls, 1);
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
