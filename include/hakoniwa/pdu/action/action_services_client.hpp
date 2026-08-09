#pragma once

#include "action_client_endpoint.hpp"
#include "action_client_state_machine.hpp"
#include "action_types.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/time_source/time_source.hpp"

#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace hakoniwa::pdu::action {

class ActionServicesClientTestPeer;

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
    // Called automatically from the owned Endpoint disconnect callback. It is
    // public for transport owners that manage the callback themselves.
    void notify_transport_disconnected();

    // The upper application owns GoalId generation. The Runtime preserves the
    // supplied non-zero ID and rejects active collisions synchronously.
    bool send_goal(const std::string& action_name,
                   const PduData& goal_pdu,
                   const GoalId& goal_id,
                   ClientGoalHandle& goal_handle_out,
                   std::uint64_t timeout_usec = 0);
    GoalSendResult send_goal_with_result(
        const std::string& action_name,
        const PduData& goal_pdu,
        const GoalId& goal_id,
        ClientGoalHandle& goal_handle_out,
        std::uint64_t timeout_usec = 0);
    bool send_cancel(const std::string& action_name,
                     const ClientGoalHandle& goal);
    ClientEventType poll(std::string& action_name, ClientEvent& event_out);
    bool create_goal_buffer(const std::string& action_name, PduData& pdu_out);

private:
    friend class ActionServicesClientTestPeer;

    // Semantic state for one Goal accepted by the Action Server. Goal
    // Response waiting and packet/slot ownership remain in the Endpoint.
    struct GoalInstance {
        ClientGoalHandle goal;
        ClientGoalContext context;
    };

    struct TransportDisconnectState {
        std::atomic<std::uint64_t> generation{0};
    };

    // One configured Action Client Endpoint and its accepted Goals.
    struct ActionInstance {
        std::string action_name;
        std::shared_ptr<IActionClientEndpoint> endpoint;
        std::vector<GoalInstance> goals;
        // Goal Requests submitted to the Endpoint but not accepted/rejected
        // yet. Endpoint owns their protocol state; Services keeps only the
        // identity required to report a confirmed transport disconnect.
        std::vector<ClientGoalHandle> pending_goals;
        std::shared_ptr<TransportDisconnectState> transport_state;
        std::uint64_t handled_disconnect_generation{0};
    };

    ActionInstance* get_action_locked(const std::string& action_name);
    GoalInstance* get_goal_locked(
        ActionInstance& action,
        const GoalId& goal_id);
    bool has_goal_locked(
        ActionInstance& action,
        const GoalId& goal_id);
    bool remove_goal_locked(
        ActionInstance& action,
        const GoalId& goal_id);
    void handle_transport_disconnected_locked(ActionInstance& action);
    void process_transport_disconnects_locked();

    ClientEventType handle_goal_response_locked(
        ActionInstance& action,
        ClientEvent& event,
        ClientEvent& event_out);
    ClientEventType handle_feedback_locked(
        ActionInstance& action,
        ClientEvent& event,
        ClientEvent& event_out);
    ClientEventType handle_cancel_response_locked(
        ActionInstance& action,
        ClientEvent& event,
        ClientEvent& event_out);
    ClientEventType handle_result_locked(
        ActionInstance& action,
        ClientEvent& event,
        ClientEvent& event_out);

    std::string node_id_;
    std::string client_name_;
    std::string config_path_;
    std::string impl_type_;
    std::uint64_t delta_time_usec_;

    std::vector<ActionInstance> actions_;
    std::deque<ClientEvent> pending_runtime_events_;
    mutable std::mutex mutex_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container_;

};

} // namespace hakoniwa::pdu::action
