#pragma once

#include <cstdint>
#include <vector>
#include "hakoniwa/pdu/endpoint_types.h"
#include "hako_srv_msgs/pdu_cpptype_conv_ServiceRequestHeader.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_ServiceResponseHeader.hpp"
#include "pdu_convertor.hpp"

namespace hakoniwa::pdu::rpc {

// Common data types
using PduData = std::vector<uint8_t>;

struct RpcRequest {
    std::string client_name;
    HakoCpp_ServiceRequestHeader header;
    PduData pdu;
};

struct RpcResponse {
    HakoCpp_ServiceResponseHeader header;
    PduData pdu;
};

// Corresponds to SERVER_API_EVENT_* in Python
enum class ServerEventType {
    NONE,
    REQUEST_IN,
    REQUEST_CANCEL
};
enum class ClientEventType {
    NONE,
    RESPONSE_IN,
    RESPONSE_CANCEL,
    RESPONSE_TIMEOUT
};

/*
 * Operation code to be set by the client when sending a service request.
 * This indicates the type of request the client wants to perform.
 * 
 * Field: HakoCpp_ServiceRequestHeader::opcode
 */
enum HakoServiceOperationCode {
    HAKO_SERVICE_OPERATION_CODE_REQUEST = 0,  // Standard service request
    HAKO_SERVICE_OPERATION_CODE_CANCEL,       // Cancel the currently active request
    HAKO_SERVICE_OPERATION_NUM
};

/*
 * Service status to be set by the server when replying to a request.
 * Indicates the internal progress/state of the requested operation.
 * 
 * Field: HakoCpp_ServiceResponseHeader::status
 */
enum HakoServiceStatus {
    HAKO_SERVICE_STATUS_NONE = 0,      // No active service
    HAKO_SERVICE_STATUS_DOING,         // Service is currently being processed
    HAKO_SERVICE_STATUS_CANCELING,     // Cancel is in progress
    HAKO_SERVICE_STATUS_DONE,          // Service has completed
    HAKO_SERVICE_STATUS_ERROR,         // An error occurred during processing
    HAKO_SERVICE_STATUS_NUM
};

/*
 * Result code to be set by the server when replying to a request.
 * This represents the outcome of the request operation.
 * 
 * Field: HakoCpp_ServiceResponseHeader::result_code
 */
enum HakoServiceResultCode {
    HAKO_SERVICE_RESULT_CODE_OK = 0,        // Request completed successfully
    HAKO_SERVICE_RESULT_CODE_ERROR,         // Execution failed due to an error
    HAKO_SERVICE_RESULT_CODE_CANCELED,      // Request was canceled by client
    HAKO_SERVICE_RESULT_CODE_INVALID,       // Request was malformed or in invalid state
    HAKO_SERVICE_RESULT_CODE_BUSY,          // Server is busy processing another request
    HAKO_SERVICE_RESULT_CODE_NUM
};

} // namespace hakoniwa::pdu::rpc
