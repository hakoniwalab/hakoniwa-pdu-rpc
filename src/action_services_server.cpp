#include "hakoniwa/pdu/action/action_services_server.hpp"

#include "hakoniwa/time_source/time_source_factory.hpp"

#include <algorithm>
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

        if (event_type != ServerEventType::CANCEL_REQUEST
            && event_type != ServerEventType::RUNTIME_CANCEL_REQUEST) {
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

        const auto goal = std::find_if(
            action.goals.begin(),
            action.goals.end(),
            [&event](const GoalInstance& instance) {
                return instance.goal.goal_id == event.goal.goal_id;
            });
        if (goal == action.goals.end()) {
            std::cerr
                << "ERROR: Action server event for unknown accepted Goal in '"
                << action.action_name
                << "'."
                << std::endl;
            action_name = action.action_name;
            event.type = ServerEventType::ERROR;
            event_out = std::move(event);
            return ServerEventType::ERROR;
        }

        const auto state_event = event_type == ServerEventType::CANCEL_REQUEST
            ? ServerGoalEvent::CANCEL_REQUEST_RECEIVED
            : ServerGoalEvent::RUNTIME_CANCEL_REQUESTED;
        const auto transition = reduce_server_goal(goal->context, state_event);
        if (transition.decision == TransitionDecision::INVARIANT_VIOLATION) {
            std::cerr
                << "ERROR: Invalid Goal state while polling Action '"
                << action.action_name
                << "'."
                << std::endl;
            action_name = action.action_name;
            event.type = ServerEventType::ERROR;
            event_out = std::move(event);
            return ServerEventType::ERROR;
        }

        if (transition.commit == TransitionCommit::IMMEDIATE) {
            goal->context = transition.next;
        }
        if (has_effect(
                transition.effects,
                TransitionEffect::NOTIFY_CANCEL_REQUEST)) {
            action_name = action.action_name;
            event_out = std::move(event);
            return event_type;
        }

        // Idempotent, ignored, or late Cancel events are consumed without an
        // Application notification. The pure reducer keeps the Goal state.
        if (has_effect(
                transition.effects,
                TransitionEffect::RECORD_DIAGNOSTIC)) {
            std::cerr
                << "WARNING: Ignored Action Cancel event for '"
                << action.action_name
                << "'."
                << std::endl;
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
    const auto action = std::find_if(
        actions_.begin(),
        actions_.end(),
        [&action_name](const ActionInstance& instance) {
            return instance.action_name == action_name;
        });
    if (action == actions_.end() || !action->endpoint) {
        return false;
    }

    const auto duplicate = std::find_if(
        action->goals.begin(),
        action->goals.end(),
        [&goal](const GoalInstance& instance) {
            return instance.goal.goal_id == goal.goal_id;
        });
    if (duplicate != action->goals.end()) {
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

} // namespace hakoniwa::pdu::action
