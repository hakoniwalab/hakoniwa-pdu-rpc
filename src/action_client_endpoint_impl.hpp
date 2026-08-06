#pragma once

#include "action_configuration.hpp"
#include "hakoniwa/pdu/action/action_client_endpoint.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/time_source.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionFeedbackHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionRequestHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionResponseHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionFeedbackHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionRequestHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionResponseHeader.hpp"
#include "hako_action_msgs/pdu_ctype_ActionFeedbackHeader.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hakoniwa::pdu::action {

class ActionClientEndpointImpl final : public IActionClientEndpoint {
public:
    ActionClientEndpointImpl(
        const std::string& action_name,
        const std::string& client_name,
        std::uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
        std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source);

    ~ActionClientEndpointImpl() override = default;

    bool initialize(const nlohmann::json& action_config) override;
    bool send_goal(const PduData& goal_pdu,
                   const GoalId& goal_id,
                   ClientGoalHandle& goal_handle_out,
                   std::uint64_t timeout_usec = 0) override;
    bool send_cancel(const ClientGoalHandle& goal) override;
    ClientEventType poll(ClientEvent& event_out) override;
    bool create_goal_buffer(PduData& pdu_out) override;
    void clear_pending_events() override;
    void reset_contexts() override;

private:
    static constexpr std::uint8_t ACTION_PROTOCOL_VERSION = 1;
    static constexpr std::uint8_t REQUEST_KIND_GOAL = 1;
    static constexpr std::uint8_t RESPONSE_KIND_GOAL = 1;
    static constexpr std::uint8_t RESPONSE_KIND_RESULT = 3;

    struct SlotRouting {
        std::size_t slot_index{0};
        hakoniwa::pdu::PduResolvedKey request;
        hakoniwa::pdu::PduResolvedKey response;
        hakoniwa::pdu::PduResolvedKey feedback;
        std::string request_packet_type;
        std::string response_packet_type;
        std::string feedback_packet_type;
        std::uint32_t request_packet_base_size{0};
        std::uint32_t response_packet_base_size{0};
        std::uint32_t feedback_packet_base_size{0};
        std::size_t request_heap_capacity{0};
        std::size_t response_heap_capacity{0};
        std::size_t feedback_heap_capacity{0};
    };

    enum class BindingState : std::uint8_t {
        AWAITING_GOAL_RESPONSE,
        GOAL_RESPONSE_TIMED_OUT,
        ACCEPTED,
    };

    struct ClientPacketBinding {
        GoalId goal_id{};
        std::size_t slot_index{0};
        BindingState state{BindingState::AWAITING_GOAL_RESPONSE};
        std::uint64_t sent_at_usec{0};
        std::uint64_t timeout_usec{0};
        std::uint32_t next_feedback_sequence{0};
    };

    enum class PendingPacketKind : std::uint8_t {
        RESPONSE,
        FEEDBACK,
    };

    struct PendingPacket {
        std::size_t slot_index{0};
        PendingPacketKind kind{PendingPacketKind::RESPONSE};
        PduData pdu;
    };

    struct PendingPacketQueue {
        std::mutex mutex;
        std::deque<PendingPacket> packets;
    };

    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::mutex mutex_;
    std::shared_ptr<PendingPacketQueue> pending_packets_;
    std::vector<SlotRouting> slot_routing_;
    std::vector<std::optional<GoalId>> slot_owners_;
    std::map<GoalId, ClientPacketBinding> packet_bindings_;
    std::optional<ActionDefinition> action_definition_;
    bool initialized_{false};

    bool create_packet_buffer(
        const std::string& packet_type,
        std::uint32_t base_size,
        std::size_t heap_capacity,
        PduData& packet_out) const;
    bool validate_packet_capacity(
        const PduData& packet,
        const std::string& packet_type,
        std::uint32_t base_size,
        std::size_t heap_capacity,
        bool require_exact_buffer_size,
        std::size_t& wire_size_out) const;
    bool write_request_header(
        PduData& packet,
        const GoalId& goal_id) const;
    bool decode_response_header(
        const PduData& packet,
        HakoCpp_ActionResponseHeader& header_out) const;
    bool decode_feedback_header(
        const PduData& packet,
        HakoCpp_ActionFeedbackHeader& header_out) const;
    void release_binding_locked(
        std::map<GoalId, ClientPacketBinding>::iterator binding);
};

} // namespace hakoniwa::pdu::action
