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

#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace hakoniwa::pdu::action {

/**
 * Initial native implementation outline for IActionServerEndpoint.
 *
 * This class deliberately contains only lifecycle and queue scaffolding. The
 * generated Action PDU conversion, Goal Context state machine, token allocation,
 * and transport callbacks are implemented in subsequent steps.
 */
class ActionServerEndpointImpl final : public IActionServerEndpoint {
public:
    ActionServerEndpointImpl(
        const std::string& action_name,
        std::uint64_t delta_time_usec,
        std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint,
        std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source);

    ~ActionServerEndpointImpl() override = default;

    bool initialize(const nlohmann::json& action_config,
                    int pdu_meta_data_size,
                    std::optional<std::string> client_node_id = std::nullopt) override;

    ServerEventType poll(ServerEvent& event_out) override;

    bool accept_goal(EventToken event_token,
                     GoalToken& goal_token_out) override;
    bool reject_goal(EventToken event_token) override;
    bool accept_cancel(EventToken event_token) override;
    bool reject_cancel(EventToken event_token) override;

    bool send_feedback(GoalToken goal_token,
                       const PduData& feedback_pdu) override;
    bool complete(GoalToken goal_token,
                  TerminalStatus status,
                  const PduData& result_pdu) override;

    void clear_pending_events() override;

private:
    static constexpr std::uint8_t ACTION_PROTOCOL_VERSION = 1;
    static constexpr std::uint8_t REQUEST_KIND_GOAL = 1;
    static constexpr std::uint8_t REQUEST_KIND_CANCEL = 2;

    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;

    // The common Header convertors deliberately operate on the prefix of an
    // Action-specific packet. Goal/Result/Feedback bodies remain opaque
    // PduData at this layer.
    hako::pdu::PduConvertor<
        HakoCpp_ActionRequestHeader,
        hako::pdu::msgs::hako_action_msgs::ActionRequestHeader>
        request_header_convertor_;
    hako::pdu::PduConvertor<
        HakoCpp_ActionResponseHeader,
        hako::pdu::msgs::hako_action_msgs::ActionResponseHeader>
        response_header_convertor_;
    hako::pdu::PduConvertor<
        HakoCpp_ActionFeedbackHeader,
        hako::pdu::msgs::hako_action_msgs::ActionFeedbackHeader>
        feedback_header_convertor_;

    std::mutex mutex_;
    std::deque<ServerEvent> pending_events_;
    std::optional<ActionDefinition> action_definition_;
    int pdu_meta_data_size_{0};
    bool initialized_{false};

    bool decode_request_header(const PduData& packet,
                               HakoCpp_ActionRequestHeader& header_out);
    bool validate_request_header(const HakoCpp_ActionRequestHeader& header) const;
    bool write_response_header(PduData& initialized_packet,
                               HakoCpp_ActionResponseHeader& header);
    bool write_feedback_header(PduData& initialized_packet,
                               HakoCpp_ActionFeedbackHeader& header);

    // TODO(endpoint contract): resolve Action packet keys, sizes, and routing,
    // then register Goal/Cancel receive callbacks with endpoint_.
    // TODO(endpoint contract): create complete ActionResponse/ActionFeedback
    // packets before write_*_header() overlays their common Header prefix.
    // TODO: add Goal Context map keyed by GoalId and GoalToken.
    // TODO: add one-shot EventToken allocation and validation.
    // TODO: centralize all Goal state changes in one locked transition function.
    // TODO: implement atomic terminal-result versus cancel-accept arbitration.
    // TODO: add Runtime-origin Cancel events for disconnect and shutdown.
};

} // namespace hakoniwa::pdu::action
