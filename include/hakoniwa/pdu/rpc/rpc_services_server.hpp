#pragma once

#include "pdu_rpc_server_endpoint.hpp"
#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace hakoniwa::pdu::rpc {

// Structure to hold the result of polling multiple services
struct ServiceEvent {
    std::string service_name;
    ServerEventType event_type;
    std::shared_ptr<IPduRpcServerEndpoint> endpoint; // Use shared_ptr for lifetime safety
};

class IRpcServicesServer {
public:
    virtual ~IRpcServicesServer() = default;

    /**
     * @brief Registers a transport-specific endpoint to a service name.
     * This is a high-level function for the front-end.
     * @param service_name The name of the service.
     * @param endpoint The underlying IPduRpcServerEndpoint to use for this service.
     * @return true if the service was successfully added, false otherwise (e.g., service_name already exists).
     */
    virtual bool add_service(const std::string& service_name, std::shared_ptr<IPduRpcServerEndpoint> endpoint) = 0;

    /**
     * @brief Polls all registered services for events.
     * @return An optional ServiceEvent if an event occurred on any service.
     */
    virtual std::optional<ServiceEvent> poll() = 0;
};

} // namespace hakoniwa::pdu::rpc
