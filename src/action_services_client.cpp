#include "hakoniwa/pdu/action/action_services_client.hpp"

#include "hakoniwa/time_source/time_source_factory.hpp"

#include <algorithm>
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
    const auto action = std::find_if(
        actions_.begin(),
        actions_.end(),
        [&action_name](const ActionInstance& instance) {
            return instance.action_name == action_name;
        });
    if (action == actions_.end() || !action->endpoint) {
        std::cerr
            << "WARNING: send_goal called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto accepted = std::find_if(
        action->goals.begin(),
        action->goals.end(),
        [&goal_id](const GoalInstance& instance) {
            return instance.goal.goal_id == goal_id;
        });
    if (accepted != action->goals.end()) {
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
    const auto action = std::find_if(
        actions_.begin(),
        actions_.end(),
        [&action_name](const ActionInstance& instance) {
            return instance.action_name == action_name;
        });
    if (action == actions_.end() || !action->endpoint) {
        std::cerr
            << "WARNING: send_cancel called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }

    const auto goal_instance = std::find_if(
        action->goals.begin(),
        action->goals.end(),
        [&goal](const GoalInstance& instance) {
            return instance.goal.goal_id == goal.goal_id;
        });
    if (goal_instance == action->goals.end()) {
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

        if (event_type != ClientEventType::GOAL_RESPONSE
            || !event.goal.valid()
            || (event.decision != Decision::ACCEPTED
                && event.decision != Decision::REJECTED)) {
            std::cerr
                << "ERROR: Unsupported or invalid Action Client event while "
                << "Goal lifecycle integration is incomplete for Action '"
                << action.action_name
                << "'."
                << std::endl;
            event.type = ClientEventType::ERROR;
            event_out = std::move(event);
            return ClientEventType::ERROR;
        }

        if (event.decision == Decision::ACCEPTED) {
            const auto duplicate = std::find_if(
                action.goals.begin(),
                action.goals.end(),
                [&event](const GoalInstance& instance) {
                    return instance.goal.goal_id == event.goal.goal_id;
                });
            if (duplicate != action.goals.end()) {
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

    return ClientEventType::NONE;
}

} // namespace hakoniwa::pdu::action
