#include "hakoniwa/pdu/action/action_services_client.hpp"

#include "hakoniwa/time_source/time_source_factory.hpp"

#include <iostream>
#include <utility>

namespace hakoniwa::pdu::action {

ActionServicesClient::ActionServicesClient(
    const std::string& node_id,
    const std::string& client_name,
    const std::string& config_path,
    const std::string& impl_type,
    std::uint64_t delta_time_usec,
    std::string time_source_type)
    : node_id_(node_id),
      client_name_(client_name),
      config_path_(config_path),
      impl_type_(impl_type),
      delta_time_usec_(delta_time_usec),
      time_source_(hakoniwa::time_source::create_time_source(
          time_source_type, delta_time_usec))
{
}

ActionServicesClient::~ActionServicesClient() = default;

ActionServicesClient::ActionInstance* ActionServicesClient::get_action_locked(
    const std::string& action_name)
{
    for (auto& action : actions_) {
        if (action.action_name == action_name) {
            return &action;
        }
    }
    return nullptr;
}

ActionServicesClient::GoalInstance* ActionServicesClient::get_goal_locked(
    ActionInstance& action,
    const GoalId& goal_id)
{
    for (auto& goal : action.goals) {
        if (goal.goal.goal_id == goal_id) {
            return &goal;
        }
    }
    return nullptr;
}

bool ActionServicesClient::has_goal_locked(
    ActionInstance& action,
    const GoalId& goal_id)
{
    return get_goal_locked(action, goal_id) != nullptr;
}

bool ActionServicesClient::remove_goal_locked(
    ActionInstance& action,
    const GoalId& goal_id)
{
    for (auto goal = action.goals.begin(); goal != action.goals.end(); ++goal) {
        if (goal->goal.goal_id == goal_id) {
            action.goals.erase(goal);
            return true;
        }
    }
    return false;
}

bool ActionServicesClient::send_goal(
    const std::string& action_name,
    const PduData& goal_pdu,
    const GoalId& goal_id,
    ClientGoalHandle& goal_handle_out,
    std::uint64_t timeout_usec)
{
    goal_handle_out = ClientGoalHandle{};
    if (action_name.empty() || goal_pdu.empty()
        || !is_valid_goal_id(goal_id)) {
        std::cerr
            << "WARNING: send_goal called with an invalid Action name, Goal "
            << "packet, or Goal ID."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: send_goal called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    if (has_goal_locked(*action, goal_id)) {
        std::cerr
            << "WARNING: send_goal called with an already accepted Goal ID "
            << "for Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    // The Endpoint owns the pre-accept Goal Response wait. A semantic
    // GoalInstance is created only when poll() observes ACCEPTED.
    return action->endpoint->send_goal(
        goal_pdu,
        goal_id,
        goal_handle_out,
        timeout_usec);
}

bool ActionServicesClient::send_cancel(
    const std::string& action_name,
    const ClientGoalHandle& goal)
{
    if (action_name.empty() || !goal.valid()) {
        std::cerr
            << "WARNING: send_cancel called with an invalid Action name or "
            << "Goal handle."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: send_cancel called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    auto* goal_instance = get_goal_locked(*action, goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "WARNING: send_cancel called for an unknown accepted Goal in "
            << "Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto transition = transition_client_goal(
        goal_instance->context,
        {ClientGoalEventType::REQUEST_CANCEL});
    if (transition.is_error()) {
        std::cerr
            << "WARNING: send_cancel rejected by the Goal state machine for "
            << "Action '"
            << action_name
            << "' (reason="
            << client_transition_error_name(transition.error)
            << ", state="
            << static_cast<int>(goal_instance->context.state)
            << ", cancel_response_pending="
            << goal_instance->context.cancel_response_pending
            << ")."
            << std::endl;
        return false;
    }
    if (!transition.transitioned()) {
        std::cerr
            << "ERROR: REQUEST_CANCEL unexpectedly produced no Goal state "
            << "transition for Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    if (!action->endpoint->send_cancel(goal)) {
        std::cerr
            << "ERROR: Failed to send Cancel Request for Action '"
            << action_name
            << "'; Client Goal state remains unchanged for an explicit retry."
            << std::endl;
        return false;
    }

    goal_instance->context = transition.next;
    return true;
}

ClientEventType ActionServicesClient::handle_goal_response_locked(
    ActionInstance& action,
    ClientEvent& event,
    ClientEvent& event_out)
{
    if (!event.goal.valid()
        || (event.decision != Decision::ACCEPTED
            && event.decision != Decision::REJECTED)) {
        std::cerr
            << "ERROR: Invalid Goal Response for Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    if (event.decision == Decision::ACCEPTED) {
        if (has_goal_locked(action, event.goal.goal_id)) {
            std::cerr
                << "ERROR: Duplicate accepted Goal Response for Action '"
                << action.action_name
                << "'."
                << std::endl;
            event.type = ClientEventType::ERROR;
            event_out = std::move(event);
            return ClientEventType::ERROR;
        }
        action.goals.push_back(GoalInstance{
            event.goal,
            ClientGoalContext{},
        });
    }

    event_out = std::move(event);
    return ClientEventType::GOAL_RESPONSE;
}

ClientEventType ActionServicesClient::handle_feedback_locked(
    ActionInstance& action,
    ClientEvent& event,
    ClientEvent& event_out)
{
    if (!event.goal.valid()) {
        std::cerr
            << "ERROR: Invalid Feedback for Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    auto* goal_instance = get_goal_locked(action, event.goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "ERROR: Feedback references an unknown accepted Goal for "
            << "Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    const auto transition = transition_client_goal(
        goal_instance->context,
        {ClientGoalEventType::FEEDBACK_RECEIVED,
         TerminalStatus::UNSPECIFIED,
         event.feedback_sequence});
    if (transition.is_error()) {
        std::cerr
            << "ERROR: Feedback rejected by the Client Goal state machine "
            << "for Action '"
            << action.action_name
            << "' (reason="
            << client_transition_error_name(transition.error)
            << ")."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }
    if (transition.is_nop()) {
        std::cerr
            << "WARNING: Ignoring delayed or out-of-order Feedback for "
            << "Action '"
            << action.action_name
            << "' (received_sequence="
            << event.feedback_sequence
            << ", expected_sequence="
            << goal_instance->context.next_feedback_sequence
            << ")."
            << std::endl;
        return ClientEventType::NONE;
    }

    goal_instance->context = transition.next;
    event_out = std::move(event);
    return ClientEventType::FEEDBACK;
}

ClientEventType ActionServicesClient::handle_cancel_response_locked(
    ActionInstance& action,
    ClientEvent& event,
    ClientEvent& event_out)
{
    if (!event.goal.valid()
        || (event.decision != Decision::ACCEPTED
            && event.decision != Decision::REJECTED)) {
        std::cerr
            << "ERROR: Invalid Cancel Response for Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    auto* goal_instance = get_goal_locked(action, event.goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "ERROR: Cancel Response references an unknown accepted Goal "
            << "for Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    const auto state_event = event.decision == Decision::ACCEPTED
        ? ClientGoalEventType::CANCEL_RESPONSE_ACCEPTED
        : ClientGoalEventType::CANCEL_RESPONSE_REJECTED;
    const auto transition = transition_client_goal(
        goal_instance->context,
        {state_event});
    if (transition.is_error()) {
        std::cerr
            << "ERROR: Cancel Response rejected by the Client Goal state "
            << "machine for Action '"
            << action.action_name
            << "' (reason="
            << client_transition_error_name(transition.error)
            << ")."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }
    if (transition.is_nop()) {
        std::cerr
            << "WARNING: Ignoring a delayed or duplicate Cancel Response "
            << "for Action '"
            << action.action_name
            << "'."
            << std::endl;
        return ClientEventType::NONE;
    }

    goal_instance->context = transition.next;
    event_out = std::move(event);
    return ClientEventType::CANCEL_RESPONSE;
}

ClientEventType ActionServicesClient::handle_result_locked(
    ActionInstance& action,
    ClientEvent& event,
    ClientEvent& event_out)
{
    if (!event.goal.valid()) {
        std::cerr
            << "ERROR: Invalid Result for Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    auto* goal_instance = get_goal_locked(action, event.goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "ERROR: Result references an unknown accepted Goal for "
            << "Action '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }

    const auto transition = transition_client_goal(
        goal_instance->context,
        {ClientGoalEventType::RESULT_RECEIVED,
         event.terminal_status});
    if (transition.is_error()) {
        std::cerr
            << "ERROR: Result rejected by the Client Goal state machine for "
            << "Action '"
            << action.action_name
            << "' (reason="
            << client_transition_error_name(transition.error)
            << ")."
            << std::endl;
        event.type = ClientEventType::ERROR;
        event_out = std::move(event);
        return ClientEventType::ERROR;
    }
    if (transition.is_nop()) {
        std::cerr
            << "WARNING: Ignoring a Result that is not valid for the current "
            << "Client Goal state in Action '"
            << action.action_name
            << "' (status="
            << static_cast<int>(event.terminal_status)
            << ")."
            << std::endl;
        return ClientEventType::NONE;
    }

    // A valid Result is the final Application event for this Goal. The
    // Endpoint has already released its packet binding and slot.
    const auto goal_id = event.goal.goal_id;
    event_out = std::move(event);
    remove_goal_locked(action, goal_id);
    return ClientEventType::RESULT;
}

ClientEventType ActionServicesClient::poll(
    std::string& action_name,
    ClientEvent& event_out)
{
    action_name.clear();
    event_out = ClientEvent{};

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& action : actions_) {
        if (!action.endpoint) {
            continue;
        }

        ClientEvent event;
        const auto event_type = action.endpoint->poll(event);
        if (event_type == ClientEventType::NONE) {
            continue;
        }

        event.type = event_type;
        event.action_name = action.action_name;
        action_name = action.action_name;

        if (event_type == ClientEventType::ERROR
            || event_type == ClientEventType::TIMEOUT) {
            event_out = std::move(event);
            return event_type;
        }

        ClientEventType handled_type = ClientEventType::ERROR;
        switch (event_type) {
        case ClientEventType::GOAL_RESPONSE:
            handled_type =
                handle_goal_response_locked(action, event, event_out);
            break;
        case ClientEventType::FEEDBACK:
            handled_type = handle_feedback_locked(action, event, event_out);
            break;
        case ClientEventType::CANCEL_RESPONSE:
            handled_type =
                handle_cancel_response_locked(action, event, event_out);
            break;
        case ClientEventType::RESULT:
            handled_type = handle_result_locked(action, event, event_out);
            break;
        default:
            std::cerr
                << "ERROR: Unsupported Action Client event for Action '"
                << action.action_name
                << "'."
                << std::endl;
            event.type = ClientEventType::ERROR;
            event_out = std::move(event);
            handled_type = ClientEventType::ERROR;
            break;
        }

        if (handled_type == ClientEventType::NONE) {
            action_name.clear();
            continue;
        }
        return handled_type;
    }

    return ClientEventType::NONE;
}

} // namespace hakoniwa::pdu::action
