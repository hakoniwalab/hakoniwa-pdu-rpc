#include "hakoniwa/pdu/hakoniwa_pdu_rpc.hpp"
#include "hakoniwa/pdu/socket_utils.hpp" // Added include
#include <iostream>

namespace hakoniwa::pdu {
    void hako_pdu_rpc_init() {
        std::cout << "hakoniwa_pdu_rpc_init()" << std::endl;
        // Call a function from hakoniwa-pdu-endpoint to verify linkage
        HakoPduErrorType error = hakoniwa::pdu::map_errno_to_error(0);
        std::cout << "map_errno_to_error(0) returned: " << error << std::endl;
    }
}
