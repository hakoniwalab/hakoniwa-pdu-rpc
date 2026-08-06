#include "action_client_endpoint_impl.hpp"

#include "pdu_size_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <span>
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

bool valid_packet_prefix(const PduData& packet, std::size_t base_size)
{
    if (packet.size() < sizeof(HakoPduMetaDataType)) {
        return false;
    }
    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, packet.data(), sizeof(metadata));
    return !HAKO_PDU_METADATA_IS_INVALID(&metadata)
        && metadata.base_off == HAKO_PDU_META_DATA_SIZE()
        && metadata.base_off <= packet.size()
        && base_size <= packet.size() - metadata.base_off
        && metadata.heap_off >= metadata.base_off + base_size
        && metadata.heap_off <= metadata.total_size
        && metadata.total_size <= packet.size();
}

} // namespace

ActionClientEndpointImpl::ActionClientEndpointImpl(
    const std::string& action_name,
    const std::string& client_name,
    std::uint64_t delta_time_usec,
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source)
    : IActionClientEndpoint(
          action_name, client_name, delta_time_usec),
      endpoint_(std::move(endpoint)),
      time_source_(std::move(time_source)),
      pending_packets_(std::make_shared<PendingPacketQueue>())
{
}

bool ActionClientEndpointImpl::create_packet_buffer(
    const std::string& packet_type,
    std::uint32_t base_size,
    std::size_t heap_capacity,
    PduData& packet_out) const
{
    if (base_size == 0
        || !supported_packet_capacity(base_size, heap_capacity)) {
        std::cerr << "ERROR: Action packet size is unavailable for type '"
                  << packet_type << "'." << std::endl;
        return false;
    }
    void* base_ptr = hako_create_empty_pdu(
        static_cast<int>(base_size),
        static_cast<int>(heap_capacity));
    if (base_ptr == nullptr) {
        return false;
    }
    const auto* metadata = hako_get_pdu_meta_data(base_ptr);
    const void* top_ptr = hako_get_top_ptr_pdu(base_ptr);
    if (metadata == nullptr || top_ptr == nullptr) {
        hako_destroy_pdu(base_ptr);
        return false;
    }
    packet_out.resize(metadata->total_size);
    std::memcpy(packet_out.data(), top_ptr, packet_out.size());
    hako_destroy_pdu(base_ptr);
    return true;
}

bool ActionClientEndpointImpl::validate_packet_capacity(
    const PduData& packet,
    const std::string& packet_type,
    std::uint32_t base_size,
    std::size_t heap_capacity,
    bool require_exact_buffer_size,
    std::size_t& wire_size_out) const
{
    wire_size_out = 0;
    if (!valid_packet_prefix(packet, base_size)
        || !supported_packet_capacity(base_size, heap_capacity)) {
        return false;
    }
    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, packet.data(), sizeof(metadata));
    const auto expected_heap_off =
        static_cast<std::size_t>(HAKO_PDU_META_DATA_SIZE())
        + aligned_size(base_size);
    const auto aligned_heap_capacity = aligned_size(heap_capacity);
    if (metadata.heap_off != expected_heap_off
        || metadata.total_size > packet.size()
        || (require_exact_buffer_size
            && metadata.total_size != packet.size())
        || metadata.total_size - metadata.heap_off
            > aligned_heap_capacity) {
        std::cerr << "ERROR: Action packet exceeds its configured contract for type '"
                  << packet_type << "'." << std::endl;
        return false;
    }
    wire_size_out = metadata.total_size;
    return true;
}

bool ActionClientEndpointImpl::write_request_header(
    PduData& packet,
    const GoalId& goal_id) const
{
    if (!valid_packet_prefix(packet, sizeof(Hako_ActionRequestHeader))) {
        return false;
    }
    auto* base_ptr = static_cast<char*>(hako_get_base_ptr_pdu(packet.data()));
    if (base_ptr == nullptr) {
        return false;
    }
    HakoCpp_ActionRequestHeader header{};
    header.version = ACTION_PROTOCOL_VERSION;
    header.request_kind = REQUEST_KIND_GOAL;
    header.reserved = {0, 0};
    header.goal_id = goal_id;
    PduDynamicMemory dynamic_memory;
    return cpp_cpp2pdu_ActionRequestHeader(
        header,
        *reinterpret_cast<Hako_ActionRequestHeader*>(base_ptr),
        dynamic_memory);
}

bool ActionClientEndpointImpl::decode_response_header(
    const PduData& packet,
    HakoCpp_ActionResponseHeader& header_out) const
{
    if (!valid_packet_prefix(packet, sizeof(Hako_ActionResponseHeader))) {
        return false;
    }
    hako::pdu::msgs::hako_action_msgs::ActionResponseHeader convertor;
    return convertor.pdu2cpp(
        reinterpret_cast<char*>(
            const_cast<std::uint8_t*>(packet.data())),
        header_out);
}

bool ActionClientEndpointImpl::decode_feedback_header(
    const PduData& packet,
    HakoCpp_ActionFeedbackHeader& header_out) const
{
    if (!valid_packet_prefix(packet, sizeof(Hako_ActionFeedbackHeader))) {
        return false;
    }
    hako::pdu::msgs::hako_action_msgs::ActionFeedbackHeader convertor;
    return convertor.pdu2cpp(
        reinterpret_cast<char*>(
            const_cast<std::uint8_t*>(packet.data())),
        header_out);
}

bool ActionClientEndpointImpl::initialize(
    const nlohmann::json& action_config)
{
    ActionDefinition definition;
    std::string error;
    if (!ActionConfigurationLoader::parse_action(
            action_config, definition, error)) {
        std::cerr << "ERROR: Failed to parse Action configuration for '"
                  << action_name_ << "': " << error << std::endl;
        return false;
    }
    if (definition.name != action_name_ || !endpoint_ || !time_source_) {
        return false;
    }

    std::vector<SlotRouting> routing(definition.slot_count);
    std::vector<std::uint8_t> masks(definition.slot_count, 0);
    std::set<std::uint32_t> channel_ids;
    for (std::size_t slot = 0; slot < routing.size(); ++slot) {
        routing[slot].slot_index = slot;
        routing[slot].request_heap_capacity = definition.buffer_heap.request_size;
        routing[slot].response_heap_capacity = definition.buffer_heap.response_size;
        routing[slot].feedback_heap_capacity = definition.buffer_heap.feedback_size;
    }

    for (const auto& channel : definition.channels) {
        if (channel.slot_index >= routing.size()
            || !channel_ids.insert(channel.channel_id).second) {
            return false;
        }
        auto& slot = routing[channel.slot_index];
        const hakoniwa::pdu::PduResolvedKey key{
            action_name_,
            static_cast<HakoPduChannelIdType>(channel.channel_id),
        };
        std::uint32_t* base_size = nullptr;
        std::size_t heap_capacity = 0;
        std::size_t minimum_header = 0;
        switch (channel.kind) {
        case ActionChannelKind::REQUEST:
            slot.request = key;
            slot.request_packet_type = channel.packet_type;
            base_size = &slot.request_packet_base_size;
            heap_capacity = slot.request_heap_capacity;
            minimum_header = sizeof(Hako_ActionRequestHeader);
            masks[channel.slot_index] |= 0x01U;
            break;
        case ActionChannelKind::RESPONSE:
            slot.response = key;
            slot.response_packet_type = channel.packet_type;
            base_size = &slot.response_packet_base_size;
            heap_capacity = slot.response_heap_capacity;
            minimum_header = sizeof(Hako_ActionResponseHeader);
            masks[channel.slot_index] |= 0x02U;
            break;
        case ActionChannelKind::FEEDBACK:
            slot.feedback = key;
            slot.feedback_packet_type = channel.packet_type;
            base_size = &slot.feedback_packet_base_size;
            heap_capacity = slot.feedback_heap_capacity;
            minimum_header = sizeof(Hako_ActionFeedbackHeader);
            masks[channel.slot_index] |= 0x04U;
            break;
        }
        if (base_size == nullptr
            || hako_pdu_get_size(channel.packet_type.c_str(), base_size) != 0
            || *base_size < minimum_header
            || !supported_packet_capacity(*base_size, heap_capacity)) {
            std::cerr << "ERROR: Action packet type is unavailable or invalid: '"
                      << channel.packet_type << "'." << std::endl;
            return false;
        }
    }
    if (std::any_of(masks.begin(), masks.end(), [](std::uint8_t mask) {
            return mask != 0x07U;
        })) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) {
            return false;
        }
        action_definition_ = std::move(definition);
        slot_routing_ = std::move(routing);
        slot_owners_.resize(slot_routing_.size());
        initialized_ = true;
    }

    std::weak_ptr<PendingPacketQueue> weak_queue = pending_packets_;
    for (const auto& slot : slot_routing_) {
        const auto slot_index = slot.slot_index;
        endpoint_->subscribe_on_recv_callback(
            slot.response,
            [weak_queue, slot_index](
                const hakoniwa::pdu::PduResolvedKey&,
                std::span<const std::byte> data) {
                if (auto queue = weak_queue.lock()) {
                    PduData packet(data.size());
                    if (!data.empty()) {
                        std::memcpy(packet.data(), data.data(), data.size());
                    }
                    std::lock_guard<std::mutex> lock(queue->mutex);
                    queue->packets.push_back(PendingPacket{
                        slot_index,
                        PendingPacketKind::RESPONSE,
                        std::move(packet)});
                }
            });
        endpoint_->subscribe_on_recv_callback(
            slot.feedback,
            [weak_queue, slot_index](
                const hakoniwa::pdu::PduResolvedKey&,
                std::span<const std::byte> data) {
                if (auto queue = weak_queue.lock()) {
                    PduData packet(data.size());
                    if (!data.empty()) {
                        std::memcpy(packet.data(), data.data(), data.size());
                    }
                    std::lock_guard<std::mutex> lock(queue->mutex);
                    queue->packets.push_back(PendingPacket{
                        slot_index,
                        PendingPacketKind::FEEDBACK,
                        std::move(packet)});
                }
            });
    }
    return true;
}

bool ActionClientEndpointImpl::create_goal_buffer(PduData& pdu_out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || slot_routing_.empty()) {
        return false;
    }
    const auto& routing = slot_routing_.front();
    return create_packet_buffer(
        routing.request_packet_type,
        routing.request_packet_base_size,
        routing.request_heap_capacity,
        pdu_out);
}

bool ActionClientEndpointImpl::send_goal(
    const PduData& goal_pdu,
    const GoalId& goal_id,
    ClientGoalHandle& goal_handle_out,
    std::uint64_t timeout_usec)
{
    goal_handle_out = ClientGoalHandle{};
    if (!is_valid_goal_id(goal_id)) {
        std::cerr << "ERROR: Action Goal ID must be non-zero." << std::endl;
        return false;
    }

    std::size_t slot_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || packet_bindings_.contains(goal_id)) {
            std::cerr << "ERROR: Action Goal ID is invalid or already active."
                      << std::endl;
            return false;
        }
        const auto free_slot = std::find_if(
            slot_owners_.begin(), slot_owners_.end(),
            [](const auto& owner) { return !owner.has_value(); });
        if (free_slot == slot_owners_.end()) {
            std::cerr << "ERROR: No free Action communication slot."
                      << std::endl;
            return false;
        }
        slot_index = static_cast<std::size_t>(
            std::distance(slot_owners_.begin(), free_slot));
        slot_owners_[slot_index] = goal_id;
        packet_bindings_.emplace(
            goal_id,
            ClientPacketBinding{
                goal_id,
                slot_index,
                BindingState::AWAITING_GOAL_RESPONSE,
                time_source_->get_microseconds(),
                timeout_usec,
            });
    }

    PduData packet = goal_pdu;
    const auto& routing = slot_routing_[slot_index];
    std::size_t wire_size = 0;
    if (!validate_packet_capacity(
            packet,
            routing.request_packet_type,
            routing.request_packet_base_size,
            routing.request_heap_capacity,
            false,
            wire_size)
        || !write_request_header(packet, goal_id)
        || endpoint_->send(
               routing.request,
               std::as_bytes(std::span(packet.data(), wire_size)))
            != HAKO_PDU_ERR_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto binding = packet_bindings_.find(goal_id);
        if (binding != packet_bindings_.end()) {
            release_binding_locked(binding);
        }
        return false;
    }

    goal_handle_out.goal_id = goal_id;
    return true;
}

bool ActionClientEndpointImpl::send_cancel(
    const ClientGoalHandle& goal)
{
    (void)goal;
    return false;
}

void ActionClientEndpointImpl::release_binding_locked(
    std::map<GoalId, ClientPacketBinding>::iterator binding)
{
    if (binding->second.slot_index < slot_owners_.size()) {
        slot_owners_[binding->second.slot_index].reset();
    }
    packet_bindings_.erase(binding);
}

ClientEventType ActionClientEndpointImpl::poll(ClientEvent& event_out)
{
    event_out = ClientEvent{};

    PendingPacket pending;
    bool has_packet = false;
    {
        std::lock_guard<std::mutex> lock(pending_packets_->mutex);
        if (!pending_packets_->packets.empty()) {
            pending = std::move(pending_packets_->packets.front());
            pending_packets_->packets.pop_front();
            has_packet = true;
        }
    }

    if (has_packet
        && pending.kind == PendingPacketKind::RESPONSE
        && pending.slot_index < slot_routing_.size()) {
        const auto& routing = slot_routing_[pending.slot_index];
        std::size_t received_size = 0;
        HakoCpp_ActionResponseHeader header{};
        if (validate_packet_capacity(
                pending.pdu,
                routing.response_packet_type,
                routing.response_packet_base_size,
                routing.response_heap_capacity,
                true,
                received_size)
            && decode_response_header(pending.pdu, header)
            && header.version == ACTION_PROTOCOL_VERSION
            && is_valid_goal_id(header.goal_id)) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto binding = packet_bindings_.find(header.goal_id);
            if (header.response_kind == RESPONSE_KIND_GOAL
                && binding != packet_bindings_.end()
                && binding->second.slot_index == pending.slot_index
                && binding->second.state
                    == BindingState::AWAITING_GOAL_RESPONSE
                && (header.status
                        == static_cast<std::uint8_t>(Decision::ACCEPTED)
                    || header.status
                        == static_cast<std::uint8_t>(Decision::REJECTED))) {
                event_out.type = ClientEventType::GOAL_RESPONSE;
                event_out.action_name = action_name_;
                event_out.goal.goal_id = header.goal_id;
                event_out.decision = static_cast<Decision>(header.status);
                event_out.pdu = std::move(pending.pdu);
                if (event_out.decision == Decision::ACCEPTED) {
                    binding->second.state = BindingState::ACCEPTED;
                } else {
                    release_binding_locked(binding);
                }
                return event_out.type;
            }
            if (header.response_kind == RESPONSE_KIND_RESULT
                && binding != packet_bindings_.end()
                && binding->second.slot_index == pending.slot_index
                && binding->second.state == BindingState::ACCEPTED
                && (header.status
                        == static_cast<std::uint8_t>(TerminalStatus::SUCCEEDED)
                    || header.status
                        == static_cast<std::uint8_t>(TerminalStatus::ABORTED))) {
                event_out.type = ClientEventType::RESULT;
                event_out.action_name = action_name_;
                event_out.goal.goal_id = header.goal_id;
                event_out.terminal_status =
                    static_cast<TerminalStatus>(header.status);
                event_out.pdu = std::move(pending.pdu);
                release_binding_locked(binding);
                return event_out.type;
            }
        }
    }

    if (has_packet
        && pending.kind == PendingPacketKind::FEEDBACK
        && pending.slot_index < slot_routing_.size()) {
        const auto& routing = slot_routing_[pending.slot_index];
        std::size_t received_size = 0;
        HakoCpp_ActionFeedbackHeader header{};
        if (validate_packet_capacity(
                pending.pdu,
                routing.feedback_packet_type,
                routing.feedback_packet_base_size,
                routing.feedback_heap_capacity,
                true,
                received_size)
            && decode_feedback_header(pending.pdu, header)
            && header.version == ACTION_PROTOCOL_VERSION
            && is_valid_goal_id(header.goal_id)) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto binding = packet_bindings_.find(header.goal_id);
            if (binding != packet_bindings_.end()
                && binding->second.slot_index == pending.slot_index
                && binding->second.state == BindingState::ACCEPTED) {
                if (header.sequence_no
                    != binding->second.next_feedback_sequence) {
                    std::cerr
                        << "ERROR: Out-of-order Action Feedback ignored for action '"
                        << action_name_
                        << "'."
                        << std::endl;
                } else {
                    event_out.type = ClientEventType::FEEDBACK;
                    event_out.action_name = action_name_;
                    event_out.goal.goal_id = header.goal_id;
                    event_out.feedback_sequence = header.sequence_no;
                    event_out.pdu = std::move(pending.pdu);
                    ++binding->second.next_feedback_sequence;
                    return event_out.type;
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return ClientEventType::NONE;
    }
    const auto now = time_source_->get_microseconds();
    for (auto binding = packet_bindings_.begin();
         binding != packet_bindings_.end(); ++binding) {
        if (binding->second.state == BindingState::AWAITING_GOAL_RESPONSE
            && binding->second.timeout_usec != 0
            && now >= binding->second.sent_at_usec
            && now - binding->second.sent_at_usec
                >= binding->second.timeout_usec) {
            event_out.type = ClientEventType::TIMEOUT;
            event_out.action_name = action_name_;
            event_out.goal.goal_id = binding->first;
            // A Goal Response timeout only means that the Client stopped
            // waiting. The Server may already own this Goal, so quarantine the
            // slot until an explicit lifecycle reset instead of reusing it for
            // another Goal.
            binding->second.state = BindingState::GOAL_RESPONSE_TIMED_OUT;
            return event_out.type;
        }
    }
    return ClientEventType::NONE;
}

void ActionClientEndpointImpl::clear_pending_events()
{
    std::lock_guard<std::mutex> lock(pending_packets_->mutex);
    pending_packets_->packets.clear();
}

void ActionClientEndpointImpl::reset_contexts()
{
    {
        std::lock_guard<std::mutex> lock(pending_packets_->mutex);
        pending_packets_->packets.clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    packet_bindings_.clear();
    for (auto& owner : slot_owners_) {
        owner.reset();
    }
}

} // namespace hakoniwa::pdu::action
