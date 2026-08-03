#pragma once

#include "action_server_endpoint.hpp"
#include "action_types.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/time_source/time_source.hpp"

#include <map>
#include <memory>
#include <string>

namespace hakoniwa::pdu::action {

class ActionServicesServer {
public:
    ActionServicesServer(const std::string& node_id,
                         const std::string& config_path,
                         const std::string& impl_type = "ActionServerEndpointImpl",
                         std::uint64_t delta_time_usec = 1000,
                         std::string time_source_type = "real");
    ~ActionServicesServer();

    bool initialize_services(
        std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container);
    bool start_all_services();
    void stop_all_services();
    void clear_all_instances();

    ServerEventType poll(std::string& action_name, ServerEvent& event_out);

    bool accept_goal(const std::string& action_name,
                     EventToken event_token,
                     GoalToken& goal_token_out);
    bool reject_goal(const std::string& action_name, EventToken event_token);
    bool accept_cancel(const std::string& action_name, EventToken event_token);
    bool reject_cancel(const std::string& action_name, EventToken event_token);
    bool send_feedback(const std::string& action_name,
                       GoalToken goal_token,
                       const PduData& feedback_pdu);
    bool complete(const std::string& action_name,
                  GoalToken goal_token,
                  TerminalStatus status,
                  const PduData& result_pdu);

private:
    std::string node_id_;
    std::string config_path_;
    std::string impl_type_;
    std::uint64_t delta_time_usec_;

    std::map<std::string, std::shared_ptr<IActionServerEndpoint>> action_endpoints_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container_;

    // TODO(codex): the Goal Context map, locking strategy, and token allocator
    // belong in the implementation. Do not expose them through this API.
};

} // namespace hakoniwa::pdu::action
