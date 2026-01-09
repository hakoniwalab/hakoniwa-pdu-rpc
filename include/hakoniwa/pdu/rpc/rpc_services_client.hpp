#pragma once

#include "rpc_types.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "rpc_client_endpoint.hpp" // For IRpcClientEndpoint
#include "rpc_client_endpoint_impl.hpp" // For RpcClientEndpointImpl
#include "hakoniwa/time_source/time_source.hpp" // For ITimeSource
#include <string>
#include <memory>
#include <map>
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace hakoniwa::pdu::rpc {

class RpcServicesClient {
public:
    RpcServicesClient(const std::string& node_id, const std::string& client_name, const std::string& config_path, const std::string& impl_type = "RpcClientEndpointImpl", uint64_t delta_time_usec = 1000, std::string time_source_type = "real");
    ~RpcServicesClient();

    bool initialize_services(std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container);
    bool start_all_services();
    void stop_all_services();
    void clear_all_instances();

    /**
     * @brief Calls an RPC service with a specified timeout.
     *
     * This method sends a request to the specified RPC service and initiates a wait for a response.
     * The response can then be retrieved by repeatedly calling the `poll` method.
     * If a response is not received within the `timeout_usec` period, a timeout event will be reported by `poll`.
     *
     * @param service_name The name of the service to call.
     * @param request_pdu The PDU data representing the request body.
     * @param timeout_usec The maximum time in microseconds to wait for a response.
     *                     A value of 0 indicates an indefinite wait (no timeout).
     * @return true if the request was successfully sent and the RPC call initiated, false otherwise.
     *         Note that `true` does not mean the service call succeeded, only that it was properly started.
     */
    bool call(const std::string& service_name, const PduData& request_pdu, uint64_t timeout_usec);
    ClientEventType poll(std::string& service_name, RpcResponse& response_out);
    bool send_cancel_request(const std::string& service_name);
    bool create_request_buffer(const std::string& service_name, PduData& pdu);
    bool create_request_buffer(const std::string& service_name, Hako_uint8 opcode, PduData& pdu);

private:
    std::string node_id_;
    std::string client_name_; // Single client identity
    std::string config_path_;
    std::string impl_type_;
    uint64_t delta_time_usec_; // For client endpoint initialization

    // Low-level PDU endpoints (node_id, endpoint_id) -> endpoint
    //TODO std::map<std::pair<std::string, std::string>, std::shared_ptr<hakoniwa::pdu::Endpoint>> pdu_endpoints_;
    // RPC client endpoints (service_name) -> endpoint
    std::map<std::string, std::shared_ptr<IRpcClientEndpoint>> rpc_endpoints_;
    std::shared_ptr<hakoniwa::time_source::ITimeSource> time_source_;
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoint_container_;
};

} // namespace hakoniwa::pdu::rpc