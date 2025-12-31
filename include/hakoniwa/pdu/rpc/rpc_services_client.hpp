#pragma once

#include "pdu_rpc_types.hpp"
#include "pdu_rpc_client_endpoint.hpp"
#include <string>
#include <future>
#include <memory>
#include <optional> // For optional return types

namespace hakoniwa::pdu::rpc {

class IRpcServicesClient {
public:
    virtual ~IRpcServicesClient() = default;

    /**
     * @brief Retrieves or creates a client endpoint for a specific service.
     * This method acts as a factory/manager for single-service client endpoints.
     * @param service_name The name of the service for which to get the client.
     * @return A shared_ptr to the IPduRpcClientEndpoint for the requested service.
     */
    virtual std::shared_ptr<IPduRpcClientEndpoint> get_client(const std::string& service_name) = 0;

    /**
     * @brief Cancels a pending request via the high-level client manager.
     * This method would route the cancellation to the appropriate single-service endpoint.
     * @param service_name The name of the service where the request was made.
     * @param request_id The ID of the request to cancel.
     * @return true if the cancellation request was successfully initiated, false otherwise.
     */
    virtual bool cancel_request(const std::string& service_name, RequestId request_id) = 0;
};

} // namespace hakoniwa::pdu::rpc
