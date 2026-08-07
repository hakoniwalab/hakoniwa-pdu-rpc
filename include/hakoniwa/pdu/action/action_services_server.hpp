#pragma once

#include "action_server_endpoint.hpp"
#include "action_server_state_machine.hpp"
#include "action_types.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/time_source/time_source.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hakoniwa::pdu::action {

class ActionServicesServerTestPeer;

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

    // poll() returns the typed ServerGoalHandle used by all subsequent Goal
    // lifecycle operations. No separate event or goal token is exposed.
    bool accept_goal(const std::string& action_name,
                     const ServerGoalHandle& goal);
    bool reject_goal(const std::string& action_name,
                     const ServerGoalHandle& goal);
    bool accept_cancel(const std::string& action_name,
                       const ServerGoalHandle& goal);
    bool reject_cancel(const std::string& action_name,
                       const ServerGoalHandle& goal);
    bool create_feedback_buffer(const std::string& action_name,
                                PduData& pdu_out);
    bool create_result_buffer(const std::string& action_name,
                              PduData& pdu_out);
    bool send_feedback(const std::string& action_name,
                       const ServerGoalHandle& goal,
                       const PduData& feedback_pdu);
    bool complete(const std::string& action_name,
                  const ServerGoalHandle& goal,
                  TerminalStatus status,
                  const PduData& result_pdu);

private:
    friend class ActionServicesServerTestPeer;

    // Semantic state for one accepted Goal. Packet binding and slot ownership
    // remain in IActionServerEndpoint.
    struct GoalInstance {
        ServerGoalHandle goal;
        ServerGoalContext context;
    };

    // One configured Action and its accepted Goal instances. slotCount bounds
    // the vector size, so the initial implementation intentionally uses a
    // simple linear lookup instead of another index or manager abstraction.
    struct ActionInstance {
        std::string action_name;
        std::shared_ptr<IActionServerEndpoint> endpoint;
        std::vector<GoalInstance> goals;
    };

    std::string node_id_;
    std::string config_path_;
    std::string impl_type_;
    std::uint64_t delta_time_usec_;

    std::vector<ActionInstance> actions_;
    mutable std::mutex mutex_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container_;
};

} // namespace hakoniwa::pdu::action
