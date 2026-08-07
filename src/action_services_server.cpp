#include "hakoniwa/pdu/action/action_services_server.hpp"

#include "hakoniwa/time_source/time_source_factory.hpp"

#include <iostream>
#include <utility>

namespace hakoniwa::pdu::action {

ActionServicesServer::ActionServicesServer(
    const std::string& node_id,
    const std::string& config_path,
    const std::string& impl_type,
    std::uint64_t delta_time_usec,
    std::string time_source_type)
    : node_id_(node_id),
      config_path_(config_path),
      impl_type_(impl_type),
      delta_time_usec_(delta_time_usec),
      time_source_(hakoniwa::time_source::create_time_source(
          time_source_type, delta_time_usec))
{
}

ActionServicesServer::~ActionServicesServer() = default;

ActionServicesServer::ActionInstance* ActionServicesServer::get_action_locked(
    const std::string& action_name)
{
    for (auto& action : actions_) {
        if (action.action_name == action_name) {
            return &action;
        }
    }
    return nullptr;
}

ActionServicesServer::GoalInstance* ActionServicesServer::get_goal_locked(
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

bool ActionServicesServer::has_goal_locked(
    ActionInstance& action,
    const GoalId& goal_id)
{
    return get_goal_locked(action, goal_id) != nullptr;
}

bool ActionServicesServer::remove_goal_locked(
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

ServerEventType ActionServicesServer::handle_cancel_event_locked(
    ActionInstance& action,
    ServerEventType event_type,
    ServerEvent& event,
    ServerEvent& event_out)
{
    auto* goal = get_goal_locked(action, event.goal.goal_id);
    if (goal == nullptr) {
        std::cerr
            << "ERROR: Action server event for unknown accepted Goal in '"
            << action.action_name
            << "'."
            << std::endl;
        event.type = ServerEventType::ERROR;
        event_out = std::move(event);
        return ServerEventType::ERROR;
    }

    const auto state_event = event_type == ServerEventType::CANCEL_REQUEST
        ? ServerGoalEvent::CANCEL_REQUEST_RECEIVED
        : ServerGoalEvent::RUNTIME_CANCEL_REQUESTED;
    const auto transition = transition_server_goal(goal->context, state_event);
    if (transition.is_error()) {
        std::cerr
            << "ERROR: Goal state transition failed while polling Action '"
            << action.action_name
            << "' (reason="
            << server_transition_error_name(transition.error)
            << ")."
            << std::endl;
        event.type = ServerEventType::ERROR;
        event_out = std::move(event);
        return ServerEventType::ERROR;
    }
    if (transition.is_nop()) {
        std::cerr
            << "WARNING: Ignored Action Cancel event for '"
            << action.action_name
            << "'."
            << std::endl;
        return ServerEventType::NONE;
    }

    goal->context = transition.next;
    event_out = std::move(event);
    return event_type;
}

ServerEventType ActionServicesServer::poll(
    std::string& action_name,
    ServerEvent& event_out)
{
    action_name.clear();
    event_out = ServerEvent{};

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& action : actions_) {
        if (!action.endpoint) {
            continue;
        }

        ServerEvent event;
        const auto event_type = action.endpoint->poll(event);
        if (event_type == ServerEventType::NONE) {
            continue;
        }

        event.type = event_type;
        event.action_name = action.action_name;
        if (event_type == ServerEventType::GOAL_REQUEST
            || event_type == ServerEventType::ERROR) {
            action_name = action.action_name;
            event_out = std::move(event);
            return event_type;
        }

        if (event_type == ServerEventType::CANCEL_REQUEST
            || event_type == ServerEventType::RUNTIME_CANCEL_REQUEST) {
            const auto handled_type = handle_cancel_event_locked(
                action, event_type, event, event_out);
            if (handled_type == ServerEventType::NONE) {
                continue;
            }
            action_name = action.action_name;
            return handled_type;
        } else {
            std::cerr
                << "ERROR: Unsupported Action server event for '"
                << action.action_name
                << "'."
                << std::endl;
            action_name = action.action_name;
            event.type = ServerEventType::ERROR;
            event_out = std::move(event);
            return ServerEventType::ERROR;
        }
    }

    return ServerEventType::NONE;
}

bool ActionServicesServer::accept_goal(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    if (action_name.empty() || !goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        return false;
    }

    if (has_goal_locked(*action, goal.goal_id)) {
        return false;
    }

    // Reserve the semantic instance before the ACCEPTED wire send. The
    // Services mutex keeps this staged entry invisible to other operations;
    // a failed send rolls it back. This avoids a post-send allocation failure
    // leaving an accepted Endpoint binding without Goal state.
    action->goals.push_back(GoalInstance{goal, ServerGoalContext{}});
    if (!action->endpoint->accept_goal(goal)) {
        action->goals.pop_back();
        return false;
    }
    return true;
}

bool ActionServicesServer::reject_goal(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    if (action_name.empty() || !goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        return false;
    }

    if (has_goal_locked(*action, goal.goal_id)) {
        return false;
    }

    // A rejected Goal never becomes a semantic GoalInstance. The Endpoint
    // owns and consumes the pending packet binding when the REJECTED response
    // is delivered successfully.
    return action->endpoint->reject_goal(goal);
}

bool ActionServicesServer::create_feedback_buffer(
    const std::string& action_name,
    PduData& pdu_out)
{
    if (action_name.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        return false;
    }
    return action->endpoint->create_feedback_buffer(pdu_out);
}

bool ActionServicesServer::create_result_buffer(
    const std::string& action_name,
    PduData& pdu_out)
{
    if (action_name.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        return false;
    }
    return action->endpoint->create_result_buffer(pdu_out);
}

bool ActionServicesServer::accept_cancel(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    if (action_name.empty() || !goal.valid()) {
        std::cerr
            << "WARNING: accept_cancel called with an invalid Action name "
            << "or Goal handle."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: accept_cancel called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    auto* goal_instance = get_goal_locked(*action, goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "WARNING: accept_cancel called for an unknown accepted Goal "
            << "in Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto cancel_origin = goal_instance->context.cancel_origin;
    const auto transition = transition_server_goal(
        goal_instance->context,
        ServerGoalEvent::ACCEPT_CANCEL);
    if (transition.is_error()) {
        std::cerr
            << "WARNING: accept_cancel rejected by the Goal state machine "
            << "for Action '"
            << action_name
            << "' (reason="
            << server_transition_error_name(transition.error)
            << ", state="
            << static_cast<int>(goal_instance->context.state)
            << ", cancel_decision_pending="
            << goal_instance->context.cancel_decision_pending
            << ", cancel_origin="
            << static_cast<int>(goal_instance->context.cancel_origin)
            << ")."
            << std::endl;
        return false;
    }
    if (!transition.transitioned()) {
        std::cerr
            << "WARNING: accept_cancel produced no Goal state transition "
            << "for Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    if (cancel_origin == CancelOrigin::CLIENT
        && !action->endpoint->accept_cancel(goal)) {
        std::cerr
            << "ERROR: Failed to send the accepted Cancel Response for "
            << "Action '"
            << action_name
            << "'; the Application Cancel decision remains pending for "
            << "an explicit retry."
            << std::endl;
        // Retain the pending Client Cancel decision so the Application may
        // explicitly retry after a synchronous send failure.
        return false;
    }

    goal_instance->context = transition.next;
    return true;
}

bool ActionServicesServer::reject_cancel(
    const std::string& action_name,
    const ServerGoalHandle& goal)
{
    if (action_name.empty() || !goal.valid()) {
        std::cerr
            << "WARNING: reject_cancel called with an invalid Action name "
            << "or Goal handle."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: reject_cancel called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    auto* goal_instance = get_goal_locked(*action, goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "WARNING: reject_cancel called for an unknown accepted Goal "
            << "in Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto cancel_origin = goal_instance->context.cancel_origin;
    const auto transition = transition_server_goal(
        goal_instance->context,
        ServerGoalEvent::REJECT_CANCEL);
    if (transition.is_error()) {
        std::cerr
            << "WARNING: reject_cancel rejected by the Goal state machine "
            << "for Action '"
            << action_name
            << "' (reason="
            << server_transition_error_name(transition.error)
            << ", state="
            << static_cast<int>(goal_instance->context.state)
            << ", cancel_decision_pending="
            << goal_instance->context.cancel_decision_pending
            << ", cancel_origin="
            << static_cast<int>(goal_instance->context.cancel_origin)
            << ")."
            << std::endl;
        return false;
    }
    if (!transition.transitioned()) {
        std::cerr
            << "WARNING: reject_cancel produced no Goal state transition "
            << "for Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    if (cancel_origin == CancelOrigin::CLIENT
        && !action->endpoint->reject_cancel(goal)) {
        std::cerr
            << "ERROR: Failed to send the rejected Cancel Response for "
            << "Action '"
            << action_name
            << "'; the Application Cancel decision remains pending for "
            << "an explicit retry."
            << std::endl;
        return false;
    }

    goal_instance->context = transition.next;
    return true;
}

bool ActionServicesServer::send_feedback(
    const std::string& action_name,
    const ServerGoalHandle& goal,
    const PduData& feedback_pdu)
{
    if (action_name.empty() || !goal.valid() || feedback_pdu.empty()) {
        std::cerr
            << "WARNING: send_feedback called with an invalid Action name, "
            << "Goal handle, or packet."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: send_feedback called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    auto* goal_instance = get_goal_locked(*action, goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "WARNING: send_feedback called for an unknown accepted Goal "
            << "in Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto transition = transition_server_goal(
        goal_instance->context,
        ServerGoalEvent::PUBLISH_FEEDBACK);
    if (transition.is_error()) {
        std::cerr
            << "WARNING: send_feedback rejected by the Goal state machine "
            << "for Action '"
            << action_name
            << "' (reason="
            << server_transition_error_name(transition.error)
            << ", state="
            << static_cast<int>(goal_instance->context.state)
            << ")."
            << std::endl;
        return false;
    }
    if (!transition.is_nop()) {
        std::cerr
            << "ERROR: PUBLISH_FEEDBACK unexpectedly changed Goal state for "
            << "Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    if (!action->endpoint->send_feedback(goal, feedback_pdu)) {
        std::cerr
            << "ERROR: Failed to send Feedback for Action '"
            << action_name
            << "'; Goal state remains unchanged."
            << std::endl;
        return false;
    }
    return true;
}

bool ActionServicesServer::complete(
    const std::string& action_name,
    const ServerGoalHandle& goal,
    TerminalStatus status,
    const PduData& result_pdu)
{
    if (action_name.empty() || !goal.valid() || result_pdu.empty()) {
        std::cerr
            << "WARNING: complete called with an invalid Action name, Goal "
            << "handle, or Result packet."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: complete called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    auto* goal_instance = get_goal_locked(*action, goal.goal_id);
    if (goal_instance == nullptr) {
        std::cerr
            << "WARNING: complete called for an unknown accepted Goal in "
            << "Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    auto state_event = ServerGoalEvent::COMPLETE_UNSPECIFIED;
    switch (status) {
    case TerminalStatus::SUCCEEDED:
        state_event = ServerGoalEvent::COMPLETE_SUCCEEDED;
        break;
    case TerminalStatus::CANCELED:
        state_event = ServerGoalEvent::COMPLETE_CANCELED;
        break;
    case TerminalStatus::ABORTED:
        state_event = ServerGoalEvent::COMPLETE_ABORTED;
        break;
    case TerminalStatus::UNSPECIFIED:
        state_event = ServerGoalEvent::COMPLETE_UNSPECIFIED;
        break;
    }

    const auto transition = transition_server_goal(
        goal_instance->context,
        state_event);
    if (transition.is_error()) {
        std::cerr
            << "WARNING: complete rejected by the Goal state machine for "
            << "Action '"
            << action_name
            << "' (reason="
            << server_transition_error_name(transition.error)
            << ", state="
            << static_cast<int>(goal_instance->context.state)
            << ", terminal_status="
            << static_cast<int>(status)
            << ")."
            << std::endl;
        return false;
    }
    if (!transition.transitioned()) {
        std::cerr
            << "ERROR: terminal completion unexpectedly produced no Goal "
            << "state transition for Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto result = action->endpoint->complete(goal, status, result_pdu);
    switch (result) {
    case CompleteResult::NOT_COMMITTED:
        std::cerr
            << "WARNING: Endpoint rejected Result before terminal commit for "
            << "Action '"
            << action_name
            << "'; Goal state remains unchanged."
            << std::endl;
        return false;

    case CompleteResult::SENT:
        remove_goal_locked(*action, goal.goal_id);
        return true;

    case CompleteResult::SEND_FAILED_AFTER_COMMIT:
        goal_instance->context = transition.next;
        std::cerr
            << "ERROR: Result send failed after terminal commit for Action '"
            << action_name
            << "'; Goal remains FINISHING and cannot be reused."
            << std::endl;
        return false;
    }

    std::cerr
        << "ERROR: Endpoint returned an unknown completion result for Action '"
        << action_name
        << "'."
        << std::endl;
    return false;
}

} // namespace hakoniwa::pdu::action
