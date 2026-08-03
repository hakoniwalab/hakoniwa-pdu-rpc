#include "action_server_endpoint_impl.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>

namespace hakoniwa::pdu::action {

bool ActionServerEndpointImpl::decode_request_header(
    const PduData& packet,
    HakoCpp_ActionRequestHeader& header_out)
{
    if (packet.empty()) {
        return false;
    }
    return request_header_convertor_.pdu2cpp(
        reinterpret_cast<char*>(const_cast<std::uint8_t*>(packet.data())),
        header_out);
}

bool ActionServerEndpointImpl::validate_request_header(
    const HakoCpp_ActionRequestHeader& header) const
{
    if (header.version != ACTION_PROTOCOL_VERSION) {
        return false;
    }
    if (header.request_kind != REQUEST_KIND_GOAL
        && header.request_kind != REQUEST_KIND_CANCEL) {
        return false;
    }
    return std::any_of(
        header.goal_id.begin(), header.goal_id.end(),
        [](std::uint8_t value) { return value != 0; });
}

bool ActionServerEndpointImpl::write_response_header(
    PduData& initialized_packet,
    HakoCpp_ActionResponseHeader& header)
{
    if (initialized_packet.empty()) {
        return false;
    }
    auto* base_ptr = static_cast<char*>(
        hako_get_base_ptr_pdu(initialized_packet.data()));
    if (base_ptr == nullptr) {
        return false;
    }

    PduDynamicMemory dynamic_memory;
    return cpp_cpp2pdu_ActionResponseHeader(
        header,
        *reinterpret_cast<Hako_ActionResponseHeader*>(base_ptr),
        dynamic_memory);
}

bool ActionServerEndpointImpl::write_feedback_header(
    PduData& initialized_packet,
    HakoCpp_ActionFeedbackHeader& header)
{
    if (initialized_packet.empty()) {
        return false;
    }
    auto* base_ptr = static_cast<char*>(
        hako_get_base_ptr_pdu(initialized_packet.data()));
    if (base_ptr == nullptr) {
        return false;
    }

    PduDynamicMemory dynamic_memory;
    return cpp_cpp2pdu_ActionFeedbackHeader(
        header,
        *reinterpret_cast<Hako_ActionFeedbackHeader*>(base_ptr),
        dynamic_memory);
}

ActionServerEndpointImpl::ActionServerEndpointImpl(
    const std::string& action_name,
    std::uint64_t delta_time_usec,
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source)
    : IActionServerEndpoint(action_name, delta_time_usec),
      endpoint_(std::move(endpoint)),
      time_source_(std::move(time_source))
{
}

bool ActionServerEndpointImpl::initialize(
    const nlohmann::json& action_config,
    int pdu_meta_data_size,
    std::optional<std::string> client_node_id)
{
    ActionDefinition parsed_definition;
    std::string parse_error;
    if (!ActionConfigurationLoader::parse_action(
            action_config, parsed_definition, parse_error)) {
        std::cerr << "ERROR: Failed to parse Action configuration for '"
                  << action_name_ << "': " << parse_error << std::endl;
        return false;
    }
    if (parsed_definition.name != action_name_) {
        std::cerr << "ERROR: Action configuration name mismatch: expected '"
                  << action_name_ << "', got '" << parsed_definition.name
                  << "'." << std::endl;
        return false;
    }
    if (pdu_meta_data_size <= 0) {
        std::cerr << "ERROR: PDU metadata size must be a positive integer."
                  << std::endl;
        return false;
    }
    if (client_node_id.has_value()
        && parsed_definition.client_endpoint.node_id != *client_node_id) {
        std::cerr << "ERROR: Action client endpoint mismatch for '"
                  << action_name_ << "': expected nodeId '" << *client_node_id
                  << "', got '" << parsed_definition.client_endpoint.node_id
                  << "'." << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!endpoint_ || !time_source_) {
        initialized_ = false;
        return false;
    }

    action_definition_ = std::move(parsed_definition);
    pdu_meta_data_size_ = pdu_meta_data_size;

    // TODO(endpoint contract): resolve generated packet base sizes and
    // transport-specific heap capacities, then register the expanded logical
    // channels with endpoint_. The logical Action configuration is fully
    // validated and retained here so this step does not need to reinterpret
    // the user-facing JSON.
    initialized_ = true;
    return true;
}

ServerEventType ActionServerEndpointImpl::poll(ServerEvent& event_out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    event_out = ServerEvent{};
    if (!initialized_ || pending_events_.empty()) {
        return ServerEventType::NONE;
    }

    event_out = std::move(pending_events_.front());
    pending_events_.pop_front();
    return event_out.type;
}

bool ActionServerEndpointImpl::accept_goal(
    EventToken event_token,
    GoalToken& goal_token_out)
{
    (void)event_token;
    goal_token_out = INVALID_GOAL_TOKEN;

    // TODO: consume a GOAL_REQUEST event token, create a Goal Context, allocate
    // a non-zero GoalToken, and emit GOAL_RESPONSE(ACCEPTED).
    return false;
}

bool ActionServerEndpointImpl::reject_goal(EventToken event_token)
{
    (void)event_token;

    // TODO: consume a GOAL_REQUEST event token and emit
    // GOAL_RESPONSE(REJECTED) without creating a Goal Context.
    return false;
}

bool ActionServerEndpointImpl::accept_cancel(EventToken event_token)
{
    (void)event_token;

    // TODO: atomically arbitrate cancel acceptance against terminal completion.
    // Client-origin cancellation emits CANCEL_RESPONSE(ACCEPTED); Runtime-origin
    // cancellation does not emit a Client response.
    return false;
}

bool ActionServerEndpointImpl::reject_cancel(EventToken event_token)
{
    (void)event_token;

    // TODO: consume the pending Cancel event. Client-origin cancellation emits
    // CANCEL_RESPONSE(REJECTED); Runtime-origin rejection remains local.
    return false;
}

bool ActionServerEndpointImpl::send_feedback(
    GoalToken goal_token,
    const PduData& feedback_pdu)
{
    (void)goal_token;
    (void)feedback_pdu;

    // TODO: validate EXECUTING state, assign the per-Goal feedback sequence,
    // encode the generated Feedback PDU, and send it through endpoint_.
    return false;
}

bool ActionServerEndpointImpl::complete(
    GoalToken goal_token,
    TerminalStatus status,
    const PduData& result_pdu)
{
    (void)goal_token;
    (void)status;
    (void)result_pdu;

    // TODO: validate the terminal status and commit exactly one immutable
    // terminal Result. If Result wins over a pending Cancel, invalidate the
    // Cancel EventToken and emit no Cancel Response.
    return false;
}

void ActionServerEndpointImpl::clear_pending_events()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.clear();
}

} // namespace hakoniwa::pdu::action
