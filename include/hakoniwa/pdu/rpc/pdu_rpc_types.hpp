#pragma once

#include <cstdint>
#include <vector>
#include "hakoniwa/pdu/endpoint_types.h" // For HakoPduErrorType

namespace hakoniwa::pdu::rpc {

// Common data types
using PduData = std::vector<uint8_t>;
using ClientId = int;
using RequestId = int64_t;

// Corresponds to API_STATUS_* in Python
enum class RpcStatus {
    NONE,
    DOING,
    CANCELING,
    DONE,
    ERROR
};

// Corresponds to API_RESULT_CODE_* in Python
enum class RpcResultCode {
    OK,
    ERROR,
    CANCELED,
    INVALID,
    BUSY
};

// Corresponds to SERVER_API_EVENT_* in Python
enum class ServerEventType {
    NONE,
    REQUEST_IN,
    REQUEST_CANCEL
};


} // namespace hakoniwa::pdu::rpc
