#pragma once

#include "hakoniwa/pdu/action/action_server_endpoint.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/time_source/time_source.hpp"

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
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;

    std::mutex mutex_;
    std::deque<ServerEvent> pending_events_;
    bool initialized_{false};

    // TODO: register generated Goal and Cancel request PDUs with endpoint_.
    // TODO: add Goal Context map keyed by GoalId and GoalToken.
    // TODO: add one-shot EventToken allocation and validation.
    // TODO: centralize all Goal state changes in one locked transition function.
    // TODO: implement atomic terminal-result versus cancel-accept arbitration.
    // TODO: add Runtime-origin Cancel events for disconnect and shutdown.
};

} // namespace hakoniwa::pdu::action
