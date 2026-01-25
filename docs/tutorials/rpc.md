# RPC Tutorial

This tutorial shows how to wire up a simple RPC server/client pair using the same configuration style as `hakoniwa-pdu-endpoint`. It assumes you already have ROS service PDU converters generated (e.g., AddTwoInts).

## Prerequisites

- C++20 compiler and CMake
- `jsonschema` for config validation (`pip install jsonschema`)

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build
```

To build the examples:

```bash
cmake -S . -B build -DHAKO_PDU_RPC_BUILD_EXAMPLES=ON
cmake --build build
```

## Configuration Files

This tutorial uses the sample configs in `config/sample/`:

- Service config: `config/sample/simple-service.json`
- Endpoints config: `config/sample/endpoints.json`
- Endpoint definitions:
  - `config/sample/server_endpoint.json`
  - `config/sample/client_endpoint.json`
- Shared cache/comm definitions:
  - `config/sample/queue.json`
  - `config/sample/tcp_server_inout_comm.json`
  - `config/sample/tcp_client_inout_comm.json`
- PDU definitions: `config/sample/pdudef.json`

Validate configs before running:

```bash
python tools/validate_configs.py config/sample/simple-service.json
```

## Server Example

```cpp
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

int main() {
    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        "server_node", "config/sample/endpoints.json");
    if (endpoints->initialize() != HAKO_PDU_ERR_OK) {
        return 1;
    }
    endpoints->start_all();

    hakoniwa::pdu::rpc::RpcServicesServer server(
        "server_node", "RpcServerEndpointImpl", "config/sample/simple-service.json", 1000);
    if (!server.initialize_services(endpoints)) {
        return 1;
    }
    server.start_all_services();

    HakoRpcServiceServerTemplateType(AddTwoInts) helper;

    while (true) {
        hakoniwa::pdu::rpc::RpcRequest req;
        auto event = server.poll(req);
        if (event == hakoniwa::pdu::rpc::ServerEventType::REQUEST_IN) {
            HakoCpp_AddTwoIntsRequest body;
            helper.get_request_body(req, body);

            HakoCpp_AddTwoIntsResponse res;
            res.sum = body.a + body.b;

            helper.reply(server, req,
                hakoniwa::pdu::rpc::HAKO_SERVICE_STATUS_DONE,
                hakoniwa::pdu::rpc::HAKO_SERVICE_RESULT_CODE_OK,
                res);
        }
    }
    return 0;
}
```

## Client Example

```cpp
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "hakoniwa/pdu/endpoint_types.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <sstream>

int main() {
    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        "client_node", "config/sample/endpoints.json");
    if (endpoints->initialize() != HAKO_PDU_ERR_OK) {
        return 1;
    }
    endpoints->start_all();

    hakoniwa::pdu::rpc::RpcServicesClient client(
        "client_node", "TestClient", "config/sample/simple-service.json", "RpcClientEndpointImpl", 1000);
    if (!client.initialize_services(endpoints)) {
        return 1;
    }
    client.start_all_services();

    HakoRpcServiceServerTemplateType(AddTwoInts) helper;

    std::cout << "Enter two integers per line (or 'q' to quit):" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "quit") {
            break;
        }
        long long a = 0;
        long long b = 0;
        std::istringstream iss(line);
        if (!(iss >> a >> b)) {
            std::cerr << "Invalid input. Expected: <a> <b>" << std::endl;
            continue;
        }

        HakoCpp_AddTwoIntsRequest req;
        req.a = a;
        req.b = b;
        helper.call(client, "Service/Add", req, 1000000);

        hakoniwa::pdu::rpc::RpcResponse res;
        hakoniwa::pdu::rpc::ClientEventType event = hakoniwa::pdu::rpc::ClientEventType::NONE;
        std::string service_name;
        while (event == hakoniwa::pdu::rpc::ClientEventType::NONE) {
            event = client.poll(service_name, res);
        }

        if (event == hakoniwa::pdu::rpc::ClientEventType::RESPONSE_IN) {
            HakoCpp_AddTwoIntsResponse body;
            helper.get_response_body(res, body);
            std::cout << "sum=" << body.sum << std::endl;
        } else {
            std::cerr << "RPC call failed or timed out (event: " << static_cast<int>(event) << ")" << std::endl;
        }
    }
    client.stop_all_services();
    endpoints->stop_all();
    return 0;
}
```

## CMake Integration

Add `hakoniwa_pdu_rpc` to your target link libraries:

```cmake
add_executable(my_rpc_app main.cpp)
target_link_libraries(my_rpc_app PRIVATE hakoniwa_pdu_rpc)
```

## Example Binaries

If you build with `-DHAKO_PDU_RPC_BUILD_EXAMPLES=ON`, you can run the sample apps:

```bash
build/examples/hakoniwa_pdu_rpc_server
build/examples/hakoniwa_pdu_rpc_client 1000000
```

The client argument is `[timeout_usec]`. Enter `a b` pairs on stdin, one per line.

## Run

1. Build your server and client applications linking against `hakoniwa_pdu_rpc`.
2. Start the server app first (it opens the server endpoint).
3. Start the client app and confirm `sum=12` is printed.

If you want to switch transports, update the comm JSON referenced by the endpoint configs (e.g., TCP to UDP) without changing C++ code.
