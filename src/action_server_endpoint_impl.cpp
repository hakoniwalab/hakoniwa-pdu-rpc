#include "action_server_endpoint_impl.hpp"

#include <nlohmann/json.hpp>
#include "pdu_size_registry.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <set>
#include <utility>

namespace hakoniwa::pdu::action {
namespace {

std::size_t aligned_size(std::size_t size)
{
    constexpr auto alignment = static_cast<std::size_t>(HAKO_ALIGNMENT_SIZE);
    return (size + alignment - 1U) & ~(alignment - 1U);
}

bool supported_packet_capacity(
    std::uint32_t base_size,
    std::size_t heap_capacity)
{
    const auto maximum = static_cast<std::size_t>(
        std::numeric_limits<int>::max());
    if (base_size > maximum || heap_capacity > maximum) {
        return false;
    }
    const auto fixed_size = static_cast<std::size_t>(HAKO_PDU_META_DATA_SIZE())
        + aligned_size(base_size);
    return fixed_size <= maximum
        && aligned_size(heap_capacity) <= maximum - fixed_size;
}

bool has_valid_packet_prefix(
    const PduData& packet,
    std::size_t required_base_size)
{
    if (packet.size() < sizeof(HakoPduMetaDataType)) {
        return false;
    }

    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, packet.data(), sizeof(metadata));
    return !HAKO_PDU_METADATA_IS_INVALID(&metadata)
        && metadata.base_off == HAKO_PDU_META_DATA_SIZE()
        && metadata.base_off <= packet.size()
        && required_base_size <= packet.size() - metadata.base_off
        && metadata.heap_off >= metadata.base_off + required_base_size
        && metadata.heap_off <= metadata.total_size
        && metadata.total_size <= packet.size();
}

std::string format_goal_id(const GoalId& goal_id)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto value : goal_id) {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    return stream.str();
}

} // namespace

bool ActionServerEndpointImpl::decode_request_header(
    const PduData& packet,
    HakoCpp_ActionRequestHeader& header_out)
{
    if (!has_valid_packet_prefix(
            packet,
            sizeof(Hako_ActionRequestHeader))) {
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
    if (!has_valid_packet_prefix(
            initialized_packet,
            sizeof(Hako_ActionResponseHeader))) {
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
    if (!has_valid_packet_prefix(
            initialized_packet,
            sizeof(Hako_ActionFeedbackHeader))) {
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

bool ActionServerEndpointImpl::create_packet_buffer(
    const std::string& packet_type,
    std::uint32_t base_size,
    std::size_t heap_capacity,
    PduData& packet_out)
{
    if (base_size == 0
        || base_size
            > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || !supported_packet_capacity(base_size, heap_capacity)) {
        std::cerr
            << "ERROR: Action packet size is unavailable for type '"
            << packet_type
            << "'."
            << std::endl;
        return false;
    }

    void* base_ptr = hako_create_empty_pdu(
        static_cast<int>(base_size),
        static_cast<int>(heap_capacity));
    if (base_ptr == nullptr) {
        return false;
    }

    auto* metadata = hako_get_pdu_meta_data(base_ptr);
    void* top_ptr = hako_get_top_ptr_pdu(base_ptr);
    if (metadata == nullptr || top_ptr == nullptr) {
        hako_destroy_pdu(base_ptr);
        return false;
    }

    packet_out.resize(metadata->total_size);
    std::memcpy(packet_out.data(), top_ptr, packet_out.size());
    hako_destroy_pdu(base_ptr);
    return true;
}

bool ActionServerEndpointImpl::create_control_response_packet(
    PduData& packet_out)
{
    if (slot_routing_.empty()) {
        return false;
    }
    const auto& routing = slot_routing_.front();
    return create_packet_buffer(
        routing.response_packet_type,
        routing.response_packet_base_size,
        0,
        packet_out);
}

bool ActionServerEndpointImpl::validate_packet_capacity(
    const PduData& packet,
    const std::string& packet_type,
    std::uint32_t base_size,
    std::size_t heap_capacity,
    bool require_exact_buffer_size,
    std::size_t& wire_size_out) const
{
    wire_size_out = 0;
    if (!has_valid_packet_prefix(packet, base_size)) {
        std::cerr
            << "ERROR: Invalid Action packet layout for type '"
            << packet_type
            << "'."
            << std::endl;
        return false;
    }

    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, packet.data(), sizeof(metadata));
    if (!supported_packet_capacity(base_size, heap_capacity)) {
        return false;
    }
    const auto expected_heap_off =
        static_cast<std::size_t>(HAKO_PDU_META_DATA_SIZE())
        + aligned_size(base_size);
    const auto aligned_heap_capacity = aligned_size(heap_capacity);
    if (metadata.heap_off != expected_heap_off
        || metadata.total_size > packet.size()
        || (require_exact_buffer_size
            && metadata.total_size != packet.size())) {
        std::cerr
            << "ERROR: Action packet offsets or total_size do not match "
            << "the generated layout for type '"
            << packet_type
            << "'."
            << std::endl;
        return false;
    }

    const auto actual_heap_storage_size = static_cast<std::size_t>(
        metadata.total_size - metadata.heap_off);
    if (actual_heap_storage_size > aligned_heap_capacity) {
        std::cerr
            << "ERROR: Action packet heap exceeds bufferHeap limit for type '"
            << packet_type
            << "': actual="
            << actual_heap_storage_size
            << ", limit="
            << aligned_heap_capacity
            << "."
            << std::endl;
        return false;
    }
    wire_size_out = metadata.total_size;
    return true;
}

bool ActionServerEndpointImpl::send_response_packet(
    const ActionPacketBinding& binding,
    std::uint8_t response_kind,
    std::uint8_t status,
    PduData packet)
{
    if (binding.slot_index >= slot_routing_.size()) {
        return false;
    }

    const auto& routing = slot_routing_[binding.slot_index];
    if (packet.empty()) {
        return false;
    }

    std::size_t wire_size = 0;
    if (!validate_packet_capacity(
            packet,
            routing.response_packet_type,
            routing.response_packet_base_size,
            routing.response_heap_capacity,
            false,
            wire_size)) {
        return false;
    }

    HakoCpp_ActionResponseHeader header{};
    header.version = ACTION_PROTOCOL_VERSION;
    header.response_kind = response_kind;
    header.status = status;
    header.reserved = 0;
    header.goal_id = binding.goal_id;
    if (!write_response_header(packet, header)) {
        return false;
    }

    return endpoint_->send(
        routing.response,
        std::as_bytes(std::span(packet.data(), wire_size)))
        == HAKO_PDU_ERR_OK;
}

void ActionServerEndpointImpl::send_goal_error_reply(
    const GoalId& goal_id,
    std::size_t slot_index,
    std::string_view reason)
{
    std::cerr
        << "ERROR: Rejecting Action Goal Request for action '"
        << action_name_
        << "', slot "
        << slot_index
        << ": "
        << reason
        << "."
        << std::endl;

    const ActionPacketBinding rejected_binding{
        goal_id,
        slot_index,
        PacketBindingState::AWAITING_GOAL_DECISION,
    };
    PduData response;
    if (!create_control_response_packet(response)) {
        std::cerr
            << "ERROR: Failed to create Action Goal rejection reply for action '"
            << action_name_
            << "', slot "
            << slot_index
            << "."
            << std::endl;
        return;
    }
    if (!send_response_packet(
            rejected_binding,
            RESPONSE_KIND_GOAL,
            static_cast<std::uint8_t>(Decision::REJECTED),
            std::move(response))) {
        std::cerr
            << "ERROR: Failed to send Action Goal rejection reply for action '"
            << action_name_
            << "', slot "
            << slot_index
            << "."
            << std::endl;
        return;
    }
}

void ActionServerEndpointImpl::log_ignored_cancel_request(
    const GoalId& goal_id,
    std::size_t slot_index,
    std::string_view reason) const
{
    std::cerr
        << "WARNING: Ignoring Action Cancel Request for action '"
        << action_name_
        << "', slot "
        << slot_index
        << ", goal_id "
        << format_goal_id(goal_id)
        << ": "
        << reason
        << ". No Cancel Response will be sent."
        << std::endl;
}

void ActionServerEndpointImpl::release_binding_locked(
    std::map<GoalId, ActionPacketBinding>::iterator binding)
{
    const auto slot_index = binding->second.slot_index;
    if (slot_index < slot_owners_.size()
        && slot_owners_[slot_index] == binding->first) {
        slot_owners_[slot_index].reset();
    }
    packet_bindings_.erase(binding);
}

ActionServerEndpointImpl::ActionServerEndpointImpl(
    const std::string& action_name,
    std::uint64_t delta_time_usec,
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source)
    : IActionServerEndpoint(action_name, delta_time_usec),
      endpoint_(std::move(endpoint)),
      time_source_(std::move(time_source)),
      pending_packets_(std::make_shared<PendingPacketQueue>())
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

    if (!endpoint_ || !time_source_) {
        return false;
    }

    std::vector<SlotRouting> parsed_routing(parsed_definition.slot_count);
    std::vector<std::uint8_t> channel_masks(parsed_definition.slot_count, 0);
    std::set<std::uint32_t> channel_ids;

    for (std::size_t slot = 0; slot < parsed_routing.size(); ++slot) {
        parsed_routing[slot].slot_index = slot;
        parsed_routing[slot].request_heap_capacity =
            parsed_definition.buffer_heap.request_size;
        parsed_routing[slot].response_heap_capacity =
            parsed_definition.buffer_heap.response_size;
        parsed_routing[slot].feedback_heap_capacity =
            parsed_definition.buffer_heap.feedback_size;
    }

    for (const auto& channel : parsed_definition.channels) {
        if (channel.slot_index >= parsed_routing.size()
            || !channel_ids.insert(channel.channel_id).second) {
            std::cerr
                << "ERROR: Invalid or duplicate Action channel routing for '"
                << action_name_
                << "'."
                << std::endl;
            return false;
        }

        auto& routing = parsed_routing[channel.slot_index];
        const hakoniwa::pdu::PduResolvedKey key{
            action_name_,
            static_cast<HakoPduChannelIdType>(channel.channel_id),
        };

        switch (channel.kind) {
        case ActionChannelKind::REQUEST:
            routing.request = key;
            routing.request_packet_type = channel.packet_type;
            if (hako_pdu_get_size(
                    channel.packet_type.c_str(),
                    &routing.request_packet_base_size) != 0
                || routing.request_packet_base_size
                    < sizeof(Hako_ActionRequestHeader)) {
                std::cerr
                    << "ERROR: Action Request packet type is unavailable "
                    << "or too small: '"
                    << channel.packet_type
                    << "'."
                    << std::endl;
                return false;
            }
            if (!supported_packet_capacity(
                    routing.request_packet_base_size,
                    routing.request_heap_capacity)) {
                std::cerr
                    << "ERROR: Action Request base size plus bufferHeap "
                    << "exceeds the supported packet range: '"
                    << channel.packet_type
                    << "'."
                    << std::endl;
                return false;
            }
            channel_masks[channel.slot_index] |= 0x01U;
            break;
        case ActionChannelKind::RESPONSE:
            routing.response = key;
            routing.response_packet_type = channel.packet_type;
            if (hako_pdu_get_size(
                    channel.packet_type.c_str(),
                    &routing.response_packet_base_size) != 0
                || routing.response_packet_base_size
                    < sizeof(Hako_ActionResponseHeader)) {
                std::cerr
                    << "ERROR: Action Response packet type is unavailable "
                    << "or too small: '"
                    << channel.packet_type
                    << "'."
                    << std::endl;
                return false;
            }
            if (!supported_packet_capacity(
                    routing.response_packet_base_size,
                    routing.response_heap_capacity)) {
                std::cerr
                    << "ERROR: Action Response base size plus bufferHeap "
                    << "exceeds the supported packet range: '"
                    << channel.packet_type
                    << "'."
                    << std::endl;
                return false;
            }
            channel_masks[channel.slot_index] |= 0x02U;
            break;
        case ActionChannelKind::FEEDBACK:
            routing.feedback = key;
            routing.feedback_packet_type = channel.packet_type;
            if (hako_pdu_get_size(
                    channel.packet_type.c_str(),
                    &routing.feedback_packet_base_size) != 0
                || routing.feedback_packet_base_size
                    < sizeof(Hako_ActionFeedbackHeader)) {
                std::cerr
                    << "ERROR: Action Feedback packet type is unavailable "
                    << "or too small: '"
                    << channel.packet_type
                    << "'."
                    << std::endl;
                return false;
            }
            if (!supported_packet_capacity(
                    routing.feedback_packet_base_size,
                    routing.feedback_heap_capacity)) {
                std::cerr
                    << "ERROR: Action Feedback base size plus bufferHeap "
                    << "exceeds the supported packet range: '"
                    << channel.packet_type
                    << "'."
                    << std::endl;
                return false;
            }
            channel_masks[channel.slot_index] |= 0x04U;
            break;
        }
    }

    if (std::any_of(
            channel_masks.begin(),
            channel_masks.end(),
            [](std::uint8_t mask) { return mask != 0x07U; })) {
        std::cerr
            << "ERROR: Action routing must contain Request, Response, and "
               "Feedback channels for every slot."
            << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        std::cerr
            << "ERROR: Action server endpoint is already initialized for '"
            << action_name_
            << "'."
            << std::endl;
        return false;
    }

    action_definition_ = std::move(parsed_definition);
    slot_routing_ = std::move(parsed_routing);
    slot_owners_.resize(slot_routing_.size());
    initialized_ = true;

    std::weak_ptr<PendingPacketQueue> weak_queue = pending_packets_;
    for (const auto& routing : slot_routing_) {
        const auto slot_index = routing.slot_index;
        endpoint_->subscribe_on_recv_callback(
            routing.request,
            [weak_queue, slot_index](
                const hakoniwa::pdu::PduResolvedKey&,
                std::span<const std::byte> data) {
                if (auto queue = weak_queue.lock()) {
                    PduData packet(data.size());
                    if (!data.empty()) {
                        std::memcpy(packet.data(), data.data(), data.size());
                    }
                    std::lock_guard<std::mutex> queue_lock(queue->mutex);
                    queue->packets.push_back(PendingPacket{
                        slot_index,
                        std::move(packet),
                    });
                }
            });
    }
    return true;
}

ServerEventType ActionServerEndpointImpl::poll(
    ServerEvent& event_out)
{
    event_out = ServerEvent{};

    PendingPacket pending_packet;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            return ServerEventType::NONE;
        }
        if (!pending_events_.empty()) {
            event_out = std::move(pending_events_.front());
            pending_events_.pop_front();
            return event_out.type;
        }
    }
    {
        std::lock_guard<std::mutex> queue_lock(pending_packets_->mutex);
        if (pending_packets_->packets.empty()) {
            return ServerEventType::NONE;
        }
        pending_packet = std::move(pending_packets_->packets.front());
        pending_packets_->packets.pop_front();
    }

    HakoCpp_ActionRequestHeader header{};
    if (pending_packet.slot_index >= slot_routing_.size()) {
        std::cerr
            << "ERROR: Action request packet references an unavailable slot "
            << pending_packet.slot_index
            << " for action '"
            << action_name_
            << "'. No response can be sent."
            << std::endl;
        return ServerEventType::NONE;
    }
    const auto& routing = slot_routing_[pending_packet.slot_index];
    std::size_t received_size = 0;
    if (!validate_packet_capacity(
            pending_packet.pdu,
            routing.request_packet_type,
            routing.request_packet_base_size,
            routing.request_heap_capacity,
            true,
            received_size)
        || !decode_request_header(pending_packet.pdu, header)
        || !validate_request_header(header)) {
        std::cerr
            << "ERROR: Invalid Action request packet received and ignored "
            << "for action '"
            << action_name_
            << "', slot "
            << pending_packet.slot_index
            << "."
            << std::endl;
        return ServerEventType::NONE;
    }

    if (header.request_kind == REQUEST_KIND_CANCEL) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto binding = packet_bindings_.find(header.goal_id);
        if (binding == packet_bindings_.end()) {
            log_ignored_cancel_request(
                header.goal_id,
                pending_packet.slot_index,
                "Goal is unknown or already completed");
            return ServerEventType::NONE;
        }
        if (binding->second.slot_index != pending_packet.slot_index) {
            log_ignored_cancel_request(
                header.goal_id,
                pending_packet.slot_index,
                "Goal is owned by a different slot");
            return ServerEventType::NONE;
        }
        if (binding->second.state
            == PacketBindingState::AWAITING_GOAL_DECISION) {
            log_ignored_cancel_request(
                header.goal_id,
                pending_packet.slot_index,
                "Goal acceptance has not completed");
            return ServerEventType::NONE;
        }
        if (binding->second.state == PacketBindingState::CANCEL_ACCEPTED) {
            log_ignored_cancel_request(
                header.goal_id,
                pending_packet.slot_index,
                "Cancel was already accepted");
            return ServerEventType::NONE;
        }
        if (binding->second.state == PacketBindingState::RESULT_COMMITTED) {
            log_ignored_cancel_request(
                header.goal_id,
                pending_packet.slot_index,
                "terminal Result is already committed");
            return ServerEventType::NONE;
        }
        if (binding->second.cancel_decision_pending) {
            log_ignored_cancel_request(
                header.goal_id,
                pending_packet.slot_index,
                "a Cancel decision is already pending");
            return ServerEventType::NONE;
        }
        binding->second.cancel_decision_pending = true;
        event_out.type = ServerEventType::CANCEL_REQUEST;
        event_out.goal.goal_id = header.goal_id;
        event_out.action_name = action_name_;
        event_out.pdu = std::move(pending_packet.pdu);
        return event_out.type;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (packet_bindings_.contains(header.goal_id)) {
            send_goal_error_reply(
                header.goal_id,
                pending_packet.slot_index,
                "duplicate Goal ID");
            return ServerEventType::NONE;
        }
        if (slot_owners_[pending_packet.slot_index].has_value()) {
            send_goal_error_reply(
                header.goal_id,
                pending_packet.slot_index,
                "slot is already owned by another Goal");
            return ServerEventType::NONE;
        } else {
            slot_owners_[pending_packet.slot_index] = header.goal_id;
            packet_bindings_.emplace(
                header.goal_id,
                ActionPacketBinding{
                    header.goal_id,
                    pending_packet.slot_index,
                    PacketBindingState::AWAITING_GOAL_DECISION,
                });
        }
    }

    event_out.type = ServerEventType::GOAL_REQUEST;
    event_out.goal.goal_id = header.goal_id;
    event_out.action_name = action_name_;
    event_out.pdu = std::move(pending_packet.pdu);
    return event_out.type;
}

bool ActionServerEndpointImpl::accept_goal(const ServerGoalHandle& goal)
{
    if (!goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }

    const auto binding = packet_bindings_.find(goal.goal_id);
    if (binding == packet_bindings_.end()
        || binding->second.state
            != PacketBindingState::AWAITING_GOAL_DECISION) {
        return false;
    }
    PduData response;
    const bool sent = create_control_response_packet(response)
        && send_response_packet(
            binding->second,
            RESPONSE_KIND_GOAL,
            static_cast<std::uint8_t>(Decision::ACCEPTED),
            std::move(response));
    if (sent) {
        binding->second.state = PacketBindingState::GOAL_ACCEPTED;
    }
    return sent;
}

bool ActionServerEndpointImpl::reject_goal(
    const ServerGoalHandle& goal)
{
    if (!goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }

    const auto binding = packet_bindings_.find(goal.goal_id);
    if (binding == packet_bindings_.end()
        || binding->second.state
            != PacketBindingState::AWAITING_GOAL_DECISION) {
        return false;
    }
    PduData response;
    const bool sent = create_control_response_packet(response)
        && send_response_packet(
            binding->second,
            RESPONSE_KIND_GOAL,
            static_cast<std::uint8_t>(Decision::REJECTED),
            std::move(response));
    if (sent) {
        release_binding_locked(binding);
    }
    return sent;
}

bool ActionServerEndpointImpl::accept_cancel(
    const ServerGoalHandle& goal)
{
    if (!goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto binding = packet_bindings_.find(goal.goal_id);
    if (binding == packet_bindings_.end()
        || binding->second.state != PacketBindingState::GOAL_ACCEPTED
        || !binding->second.cancel_decision_pending) {
        return false;
    }
    PduData response;
    const bool sent = create_control_response_packet(response)
        && send_response_packet(
            binding->second,
            RESPONSE_KIND_CANCEL,
            static_cast<std::uint8_t>(Decision::ACCEPTED),
            std::move(response));
    if (sent) {
        binding->second.state = PacketBindingState::CANCEL_ACCEPTED;
        binding->second.cancel_decision_pending = false;
    }
    return sent;
}

bool ActionServerEndpointImpl::create_result_buffer(PduData& pdu_out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || slot_routing_.empty()) {
        return false;
    }
    const auto& routing = slot_routing_.front();
    return create_packet_buffer(
        routing.response_packet_type,
        routing.response_packet_base_size,
        routing.response_heap_capacity,
        pdu_out);
}

bool ActionServerEndpointImpl::create_feedback_buffer(PduData& pdu_out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || slot_routing_.empty()) {
        return false;
    }
    const auto& routing = slot_routing_.front();
    return create_packet_buffer(
        routing.feedback_packet_type,
        routing.feedback_packet_base_size,
        routing.feedback_heap_capacity,
        pdu_out);
}

bool ActionServerEndpointImpl::reject_cancel(
    const ServerGoalHandle& goal)
{
    if (!goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto binding = packet_bindings_.find(goal.goal_id);
    if (binding == packet_bindings_.end()
        || binding->second.state != PacketBindingState::GOAL_ACCEPTED
        || !binding->second.cancel_decision_pending) {
        return false;
    }
    PduData response;
    const bool sent = create_control_response_packet(response)
        && send_response_packet(
            binding->second,
            RESPONSE_KIND_CANCEL,
            static_cast<std::uint8_t>(Decision::REJECTED),
            std::move(response));
    if (sent) {
        binding->second.cancel_decision_pending = false;
    }
    return sent;
}

bool ActionServerEndpointImpl::send_feedback(
    const ServerGoalHandle& goal,
    const PduData& feedback_pdu)
{
    if (!goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto binding = packet_bindings_.find(goal.goal_id);
    if (binding == packet_bindings_.end()
        || (binding->second.state != PacketBindingState::GOAL_ACCEPTED
            && binding->second.state != PacketBindingState::CANCEL_ACCEPTED)
        || binding->second.slot_index >= slot_routing_.size()) {
        return false;
    }

    const auto& routing = slot_routing_[binding->second.slot_index];
    PduData packet = feedback_pdu;
    std::size_t wire_size = 0;
    if (!validate_packet_capacity(
            packet,
            routing.feedback_packet_type,
            routing.feedback_packet_base_size,
            routing.feedback_heap_capacity,
            false,
            wire_size)) {
        return false;
    }

    HakoCpp_ActionFeedbackHeader header{};
    header.version = ACTION_PROTOCOL_VERSION;
    header.reserved = {0, 0, 0};
    header.goal_id = goal.goal_id;
    header.sequence_no = binding->second.next_feedback_sequence;
    if (!write_feedback_header(packet, header)) {
        return false;
    }
    if (endpoint_->send(
            routing.feedback,
            std::as_bytes(std::span(packet.data(), wire_size)))
        != HAKO_PDU_ERR_OK) {
        return false;
    }

    // Sequence numbers describe successfully committed wire sends. A failed
    // send keeps the same number available for an explicit retry.
    ++binding->second.next_feedback_sequence;
    return true;
}

bool ActionServerEndpointImpl::complete(
    const ServerGoalHandle& goal,
    TerminalStatus status,
    const PduData& result_pdu)
{
    if (!goal.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto binding = packet_bindings_.find(goal.goal_id);
    if (binding == packet_bindings_.end()) {
        return false;
    }
    const bool executing_completion =
        binding->second.state == PacketBindingState::GOAL_ACCEPTED
        && (status == TerminalStatus::SUCCEEDED
            || status == TerminalStatus::ABORTED);
    const bool canceling_completion =
        binding->second.state == PacketBindingState::CANCEL_ACCEPTED
        && (status == TerminalStatus::CANCELED
            || status == TerminalStatus::ABORTED);
    if (!executing_completion && !canceling_completion) {
        return false;
    }
    if (binding->second.slot_index >= slot_routing_.size()) {
        return false;
    }
    const auto& routing = slot_routing_[binding->second.slot_index];
    std::size_t wire_size = 0;
    if (!validate_packet_capacity(
            result_pdu,
            routing.response_packet_type,
            routing.response_packet_base_size,
            routing.response_heap_capacity,
            false,
            wire_size)) {
        // Invalid Application input must not commit the Goal. The caller may
        // correct the encoded Result and call complete() again.
        return false;
    }

    binding->second.state = PacketBindingState::RESULT_COMMITTED;
    binding->second.cancel_decision_pending = false;
    const bool sent = send_response_packet(
        binding->second,
        RESPONSE_KIND_RESULT,
        static_cast<std::uint8_t>(status),
        result_pdu);
    if (sent) {
        release_binding_locked(binding);
    }
    return sent;
}

void ActionServerEndpointImpl::clear_pending_events()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.clear();
    std::lock_guard<std::mutex> queue_lock(pending_packets_->mutex);
    pending_packets_->packets.clear();
}

void ActionServerEndpointImpl::reset_contexts()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.clear();
    packet_bindings_.clear();
    for (auto& owner : slot_owners_) {
        owner.reset();
    }
    std::lock_guard<std::mutex> queue_lock(pending_packets_->mutex);
    pending_packets_->packets.clear();
}

} // namespace hakoniwa::pdu::action
