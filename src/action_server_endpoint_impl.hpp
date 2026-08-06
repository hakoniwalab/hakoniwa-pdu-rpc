#pragma once

#include "action_configuration.hpp"
#include "hakoniwa/pdu/action/action_server_endpoint.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/time_source.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionFeedbackHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionRequestHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionResponseHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionFeedbackHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionRequestHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionResponseHeader.hpp"
#include "pdu_convertor.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hakoniwa::pdu::action {

/**
 * Server-side Action packet endpoint.
 *
 * Maps upper-layer Goal transactions to Endpoint slots and packet channels,
 * owns packet queues and Header conversion, and delegates byte delivery to
 * hakoniwa-pdu-endpoint. The EXECUTING/CANCELING/FINISHING Protocol state
 * machine belongs to the upper Goal transaction layer.
 */
class ActionServerEndpointImpl final : public IActionServerEndpoint {
public:
    ActionServerEndpointImpl(
        const std::string& action_name,
        std::uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
        std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source);

    ~ActionServerEndpointImpl() override = default;

    bool initialize(const nlohmann::json& action_config) override;

    ServerEventType poll(ServerEvent& event_out) override;

    bool accept_goal(const ServerGoalHandle& goal) override;
    bool reject_goal(const ServerGoalHandle& goal) override;

    bool accept_cancel(const ServerGoalHandle& goal) override;
    bool reject_cancel(const ServerGoalHandle& goal) override;

    bool create_result_buffer(PduData& pdu_out) override;
    bool create_feedback_buffer(PduData& pdu_out) override;

    bool send_feedback(
        const ServerGoalHandle& goal,
        const PduData& feedback_pdu) override;

    bool complete(
        const ServerGoalHandle& goal,
        TerminalStatus status,
        const PduData& result_pdu) override;

    void clear_pending_events() override;
    void reset_contexts() override;

private:
    static constexpr std::uint8_t ACTION_PROTOCOL_VERSION = 1;
    static constexpr std::uint8_t REQUEST_KIND_GOAL = 1;
    static constexpr std::uint8_t REQUEST_KIND_CANCEL = 2;
    static constexpr std::uint8_t RESPONSE_KIND_GOAL = 1;
    static constexpr std::uint8_t RESPONSE_KIND_CANCEL = 2;
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

    struct PendingPacket {
        std::size_t slot_index{0};
        PduData pdu;
    };

    struct PendingPacketQueue {
        std::mutex mutex;
        std::deque<PendingPacket> packets;
    };

    enum class PacketBindingState : std::uint8_t {
        AWAITING_GOAL_DECISION,
        GOAL_ACCEPT_RESPONSE_SENDING,
        GOAL_REJECT_RESPONSE_SENDING,
        GOAL_ACCEPTED,
        CANCEL_ACCEPT_RESPONSE_SENDING,
        CANCEL_REJECT_RESPONSE_SENDING,
        CANCEL_ACCEPTED,
        RESULT_COMMITTED,
    };

    // Transport-facing association between an upper-layer Goal transaction
    // and the slot/channels on which its packets are exchanged. This is not
    // the Action Protocol EXECUTING/CANCELING/FINISHING state machine.
    struct ActionPacketBinding {
        GoalId goal_id{};
        std::size_t slot_index{0};
        PacketBindingState state{PacketBindingState::AWAITING_GOAL_DECISION};
        std::uint32_t next_feedback_sequence{0};
        bool cancel_decision_pending{false};
    };

    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;

    // The common Header convertors deliberately operate on the prefix of an
    // Action-specific packet. Goal/Result/Feedback bodies remain opaque
    // PduData at this layer.
    hako::pdu::PduConvertor<
        HakoCpp_ActionRequestHeader,
        hako::pdu::msgs::hako_action_msgs::ActionRequestHeader>
        request_header_convertor_;

    std::mutex mutex_;
    std::deque<ServerEvent> pending_events_;
    std::shared_ptr<PendingPacketQueue> pending_packets_;
    std::vector<SlotRouting> slot_routing_;
    std::vector<std::optional<GoalId>> slot_owners_;
    std::map<GoalId, ActionPacketBinding> packet_bindings_;
    std::optional<ActionDefinition> action_definition_;
    bool initialized_{false};

    bool decode_request_header(
        const PduData& packet,
        HakoCpp_ActionRequestHeader& header_out);

    bool validate_request_header(
        const HakoCpp_ActionRequestHeader& header) const;

    bool write_response_header(
        PduData& initialized_packet,
        HakoCpp_ActionResponseHeader& header);

    bool write_feedback_header(
        PduData& initialized_packet,
        HakoCpp_ActionFeedbackHeader& header);

    bool create_packet_buffer(
        const std::string& packet_type,
        std::uint32_t base_size,
        std::size_t heap_capacity,
        PduData& packet_out);

    bool create_control_response_packet(PduData& packet_out);

    bool validate_packet_capacity(
        const PduData& packet,
        const std::string& packet_type,
        std::uint32_t base_size,
        std::size_t heap_capacity,
        bool require_exact_buffer_size,
        std::size_t& wire_size_out) const;

    bool send_response_packet(
        const ActionPacketBinding& binding,
        std::uint8_t response_kind,
        std::uint8_t status,
        PduData packet = {});

    void release_binding_locked(
        std::map<GoalId, ActionPacketBinding>::iterator binding);

    // TODO(binding): retain the ingress Endpoint/connection association when
    // mux or other multi-session transports are connected.
    // TODO(runtime error): expose synchronous Endpoint send failures through
    // the specified Runtime Error event without converting them to ABORTED.
};

} // namespace hakoniwa::pdu::action
