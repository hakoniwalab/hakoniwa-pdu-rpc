#pragma once

#include "action_client_endpoint.hpp"
#include "action_types.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/time_source/time_source.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace hakoniwa::pdu::action {

class ActionServicesClient {
public:
    ActionServicesClient(const std::string& node_id,
                         const std::string& client_name,
                         const std::string& config_path,
                         const std::string& impl_type = "ActionClientEndpointImpl",
                         std::uint64_t delta_time_usec = 1000,
                         std::string time_source_type = "real");
    ~ActionServicesClient();

    bool initialize_services(
        std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container);
    bool start_all_services();
    void stop_all_services();
    void clear_all_instances();

    // Ordinary callers receive an opaque-style GoalHandle and do not need to
    // construct or retain GoalId directly. Adapters may request a specific ID.
    bool send_goal(const std::string& action_name,
                   const PduData& goal_pdu,
                   ClientGoalHandle& goal_handle_out,
                   std::optional<GoalId> requested_goal_id = std::nullopt,
                   std::uint64_t timeout_usec = 0);
    bool send_cancel(const std::string& action_name,
                     const ClientGoalHandle& goal);
    ClientEventType poll(std::string& action_name, ClientEvent& event_out);
    bool create_goal_buffer(const std::string& action_name, PduData& pdu_out);

private:
    std::string node_id_;
    std::string client_name_;
    std::string config_path_;
    std::string impl_type_;
    std::uint64_t delta_time_usec_;

    std::map<std::string, std::shared_ptr<IActionClientEndpoint>> action_endpoints_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container_;

    // TODO(codex): decide whether round-robin polling is sufficient or whether
    // a ready queue is needed. This is an implementation detail, not API.
};

} // namespace hakoniwa::pdu::action
