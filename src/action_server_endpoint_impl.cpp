#include "action_server_endpoint_impl.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <utility>

namespace hakoniwa::pdu::action {

bool ActionServerEndpointImpl::decode_request_header(
    const PduData& packet,
    HakoCpp_ActionRequestHeader& header_out)
{
    if (packet.empty()) {
        return false;
    }

    return request_header_convertor_.pdu2cpp(
        reinterpret_cast<char*>(
            const_cast<std::uint8_t*>(packet.data())),
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
        header.goal_id.begin(),
        header.goal_id.end(),
        [](std::uint8_t value) {
            return value != 0;
        });
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
    const nlohmann::json& action_config)
{
    ActionDefinition parsed_definition;
    std::string parse_error;

    if (!ActionConfigurationLoader::parse_action(
            action_config,
            parsed_definition,
            parse_error)) {
        std::cerr
            << "ERROR: Failed to parse Action configuration for '"
            << action_name_
            << "': "
            << parse_error
            << std::endl;
        return false;
    }

    if (parsed_definition.name != action_name_) {
        std::cerr
            << "ERROR: Action configuration name mismatch: expected '"
            << action_name_
            << "', got '"
            << parsed_definition.name
            << "'."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!endpoint_ || !time_source_) {
        initialized_ = false;
        return false;
    }

    action_definition_ = std::move(parsed_definition);

    // TODO(endpoint contract):
    // Resolve the Action request, response, and feedback packet definitions
    // from endpoint_, then register the receive callbacks and routing for each
    // configured slot.
    //
    // Packet sizes and transport-specific metadata are owned by Endpoint and
    // its PDU/transport configuration. They are not part of the Action API.
    initialized_ = true;
    return true;
}

ServerEventType ActionServerEndpointImpl::poll(
    ServerEvent& event_out)
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

bool ActionServerEndpointImpl::accept_goal(const ServerGoalHandle& goal)
{
    (void)goal;

    // TODO:
    return false;
}

bool ActionServerEndpointImpl::reject_goal(
    const ServerGoalHandle& goal)
{
    (void)goal;

    // TODO:
    // - validate goal_id
    // - find the pending GOAL_REQUEST Context keyed by goal_id
    // - ensure that the Goal decision is still pending
    // - send GOAL_RESPONSE(REJECTED)
    // - release the pending Goal Context after delivery responsibility ends
    return false;
}

bool ActionServerEndpointImpl::accept_cancel(
    const ServerGoalHandle& goal)
{
    (void)goal;

    // TODO:
    // - find the accepted Goal Context keyed by goal_id
    // - ensure that a Cancel decision is pending
    // - atomically arbitrate Cancel acceptance against terminal completion
    // - transition EXECUTING -> CANCELING
    // - send CANCEL_RESPONSE(ACCEPTED) for a Client-origin Cancel
    //
    // Runtime-origin cancellation does not emit a Client Cancel Response.
    return false;
}

bool ActionServerEndpointImpl::reject_cancel(
    const ServerGoalHandle& goal)
{
    (void)goal;

    // TODO:
    // - find the accepted Goal Context keyed by goal_id
    // - ensure that a Cancel decision is pending
    // - clear the pending Cancel decision
    // - keep the Goal in EXECUTING
    // - send CANCEL_RESPONSE(REJECTED) for a Client-origin Cancel
    //
    // Runtime-origin rejection remains local to the Runtime.
    return false;
}

bool ActionServerEndpointImpl::send_feedback(
    const ServerGoalHandle& goal,
    const PduData& feedback_pdu)
{
    (void)goal;
    (void)feedback_pdu;

    // TODO:
    // - find the Goal Context keyed by goal_id
    // - require the Goal to be EXECUTING
    // - assign and increment the per-Goal feedback sequence
    // - write the Action Feedback Header
    // - send the complete generated Feedback PDU through endpoint_
    return false;
}

bool ActionServerEndpointImpl::complete(
    const ServerGoalHandle& goal,
    TerminalStatus status,
    const PduData& result_pdu)
{
    (void)goal;
    (void)status;
    (void)result_pdu;

    // TODO:
    // - find the Goal Context keyed by goal_id
    // - validate status against the current Goal state
    // - commit exactly one immutable terminal Result
    // - atomically arbitrate terminal completion against Cancel acceptance
    // - if Result wins, close any pending Cancel decision without emitting a
    //   Cancel Response
    // - transition the Goal Context to FINISHING
    // - release the Context after Result delivery responsibility ends
    return false;
}

void ActionServerEndpointImpl::clear_pending_events()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.clear();
}

} // namespace hakoniwa::pdu::action