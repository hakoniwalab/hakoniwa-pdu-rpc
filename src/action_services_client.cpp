#include "hakoniwa/pdu/action/action_services_client.hpp"

#include "action_client_endpoint_impl.hpp"
#include "action_configuration.hpp"
#include "hakoniwa/time_source/time_source_factory.hpp"

#include <fstream>
#include <iostream>
#include <utility>

#include <nlohmann/json.hpp>

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

ActionServicesClient::~ActionServicesClient()
{
    stop_all_services();
}

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

bool ActionServicesClient::initialize_services(
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container)
{
    if (!endpoint_container) {
        std::cerr << "ERROR: Action Client EndpointContainer is required."
                  << std::endl;
        return false;
    }

    std::ifstream stream(config_path_);
    if (!stream.is_open()) {
        std::cerr
            << "ERROR: Failed to open Action configuration: "
            << config_path_
            << std::endl;
        return false;
    }

    nlohmann::json root;
    try {
        stream >> root;
    } catch (const nlohmann::json::exception& error) {
        std::cerr
            << "ERROR: Failed to parse Action configuration JSON: "
            << error.what()
            << std::endl;
        return false;
    }

    ActionConfiguration configuration;
    std::string configuration_error;
    if (!ActionConfigurationLoader::parse(
            root, configuration, configuration_error)) {
        std::cerr
            << "ERROR: Invalid Action configuration: "
            << configuration_error
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!actions_.empty()) {
        std::cerr
            << "ERROR: Action Client Services are already initialized."
            << std::endl;
        return false;
    }

    std::vector<ActionInstance> initialized_actions;
    const auto& action_entries = root.at("actions");
    for (std::size_t index = 0; index < configuration.actions.size(); ++index) {
        const auto& definition = configuration.actions[index];
        if (definition.client_endpoint.node_id != node_id_) {
            continue;
        }

        const auto& endpoint_id =
            definition.client_endpoint.endpoint_id.empty()
            ? definition.client_endpoint.node_id
            : definition.client_endpoint.endpoint_id;
        auto pdu_endpoint = endpoint_container->ref(endpoint_id);
        if (!pdu_endpoint) {
            std::cerr
                << "ERROR: Client Endpoint '"
                << endpoint_id
                << "' was not found for Action '"
                << definition.name
                << "'."
                << std::endl;
            return false;
        }

        if (impl_type_ != "ActionClientEndpointImpl") {
            std::cerr
                << "ERROR: Unsupported Action Client Endpoint implementation: "
                << impl_type_
                << std::endl;
            return false;
        }

        auto action_endpoint = std::make_shared<ActionClientEndpointImpl>(
            definition.name,
            client_name_,
            delta_time_usec_,
            std::move(pdu_endpoint),
            time_source_);
        if (!action_endpoint->initialize(action_entries.at(index))) {
            std::cerr
                << "ERROR: Failed to initialize Action Client Endpoint for '"
                << definition.name
                << "'."
                << std::endl;
            return false;
        }
        initialized_actions.push_back(ActionInstance{
            definition.name,
            std::move(action_endpoint),
            {},
        });
    }

    endpoint_container_ = std::move(endpoint_container);
    actions_ = std::move(initialized_actions);
    return true;
}

bool ActionServicesClient::start_all_services()
{
    // EndpointContainer lifecycle is owned by the caller. Action Services
    // have no additional worker to start in the initial implementation.
    return true;
}

void ActionServicesClient::stop_all_services()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& action : actions_) {
        if (action.endpoint) {
            action.endpoint->clear_pending_events();
        }
    }
}

void ActionServicesClient::clear_all_instances()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& action : actions_) {
        action.goals.clear();
        if (action.endpoint) {
            action.endpoint->reset_contexts();
        }
    }
}

bool ActionServicesClient::send_goal(
    const std::string& action_name,
    const PduData& goal_pdu,
    const GoalId& goal_id,
    ClientGoalHandle& goal_handle_out,
    std::uint64_t timeout_usec)
{
    return send_goal_with_result(
               action_name,
               goal_pdu,
               goal_id,
               goal_handle_out,
               timeout_usec)
        == GoalSendResult::SUCCESS;
}

GoalSendResult ActionServicesClient::send_goal_with_result(
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
        return GoalSendResult::INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: send_goal called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return GoalSendResult::ACTION_NOT_FOUND;
    }

    if (has_goal_locked(*action, goal_id)) {
        std::cerr
            << "WARNING: send_goal called with an already accepted Goal ID "
            << "for Action '"
            << action_name
            << "'."
            << std::endl;
        return GoalSendResult::DUPLICATE_GOAL;
    }

    // The Endpoint owns the pre-accept Goal Response wait. A semantic
    // GoalInstance is created only when poll() observes ACCEPTED.
    return action->endpoint->send_goal_with_result(
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

bool ActionServicesClient::create_goal_buffer(
    const std::string& action_name,
    PduData& pdu_out)
{
    pdu_out.clear();
    if (action_name.empty()) {
        std::cerr
            << "WARNING: create_goal_buffer called with an empty Action name."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* action = get_action_locked(action_name);
    if (action == nullptr || !action->endpoint) {
        std::cerr
            << "WARNING: create_goal_buffer called for unknown Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }
    if (!action->endpoint->create_goal_buffer(pdu_out)) {
        pdu_out.clear();
        std::cerr
            << "ERROR: Failed to create Goal buffer for Action '"
            << action_name
            << "'."
            << std::endl;
        return false;
    }
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
