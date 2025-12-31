#pragma once

#include "pdu_rpc_types.hpp"
#include <string>
#include <future>
#include <chrono>

namespace hakoniwa::pdu::rpc {

class IPduRpcClientEndpoint {
public:
    virtual ~IPduRpcClientEndpoint() = default;

    virtual bool initialize_services(const std::string& service_path, uint64_t delta_time_usec) = 0;
    virtual void sleep(uint64_t time_usec) = 0;

    virtual ClientId register_client(const std::string& service_name, const std::string& client_name) = 0;

    /**
     * @brief Sends an RPC request asynchronously.
     * @param pdu The PDU data for the request.
     * @param timeout The timeout for the request.
     * @return A future that will eventually hold the PDU data of the response.
     */
    virtual std::future<PduData> async_call(const PduData& pdu, uint64_t timeout_usec) = 0;

    /**
     * @brief Cancels a pending request.
     * Note: Cancellation is best-effort and may not be supported by all transport implementations.
     * @param request_id The ID of the request to cancel.
     */
    virtual void cancel_request(RequestId request_id) = 0;
};

} // namespace hakoniwa::pdu::rpc
