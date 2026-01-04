# Hakoniwa PDU-RPC

`hakoniwa-pdu-rpc` is a C++ library that provides a framework for remote procedure calls (RPC) built on Hakoniwa's PDU (Protocol Data Unit) communication layer. It is designed for scenarios where reliable, request-response style communication is needed between distributed components in the Hakoniwa ecosystem.

## Overview

This library allows a "server" component to offer one or more services and "client" components to invoke those services. Communication is defined by a central JSON configuration file, and the data structures (PDUs) for requests and responses are based on ROS-compatible message definitions.

It provides a higher-level abstraction over the raw `hakoniwa-pdu-endpoint` library, handling the boilerplate of request IDs, timeouts, and state management for RPC calls.

## Features

*   **Service-Oriented RPC:** Define and manage multiple RPC services within a single server.
*   **Multi-Client Support:** A single service can be called by multiple, uniquely-named clients.
*   **Configuration-Driven:** Define all services, endpoints, PDUs, and communication channels in a single JSON file.
*   **Simplified Usage with Helpers:** A template-based helper (`HakoRpcServiceServerTemplateType`) is provided to automatically handle PDU packing/unpacking for specific ROS service types.
*   **Transport Agnostic:** Leverages `hakoniwa-pdu-endpoint` to run over different transports (TCP, UDP, Shared Memory) without changing user code.

## How to Build

This project uses CMake.

```bash
# Create a build directory
mkdir build
cd build

# Generate makefiles and build
cmake ..
make
```

## Core Concepts

### Services
A "service" is a remote procedure that a client can call. It has a unique name (e.g., `"Service/Add"`) and is defined by a request PDU and a response PDU. A server implements the logic for a service, and a client calls it.

### Configuration
The entire RPC topology is defined in a central JSON file. This file specifies:
1.  `endpoints`: The low-level communication endpoints (e.g., TCP server/client addresses).
2.  `services`: The list of available RPC services, including their names, PDU types, PDU sizes, and which clients are allowed to call them.

### RPC Service Helper
To simplify development, the library provides a template helper class `HakoRpcServiceServerTemplateType`. When instantiated with a ROS service type (e.g., `HakoRpcServiceServerTemplateType(AddTwoInts)`), it provides methods to easily:
*   `call()`: Send a typed C++ request structure from the client.
*   `get_request_body()`: Extract a typed C++ request structure on the server.
*   `reply()`: Send a typed C++ response structure from the server.
*   `get_response_body()`: Extract a typed C++ response structure on the client.

This avoids the need for manual byte-level PDU manipulation.

## API Reference

The primary entry points for user applications are the `RpcServicesServer` and `RpcServicesClient` classes.

### `RpcServicesClient`
Manages the client side of one or more RPC services.

*   `RpcServicesClient(node_id, client_name, config_path, ...)`: Constructor.
    *   `node_id`: The ID of the node this client is running on (must match an ID in the config).
    *   `client_name`: A unique name for this client instance (must match a client name in the config).
    *   `config_path`: Path to the service configuration JSON file.
*   `bool initialize_services()`: Reads the config and initializes all services this client can call.
*   `bool start_all_services()`: Starts the underlying PDU endpoints.
*   `bool call(service_name, request_pdu, timeout_usec)`: Makes an RPC call. (Note: Using the service helper is recommended over this).
*   `ClientEventType poll(service_name, response_out)`: Polls for responses or other events.

### `RpcServicesServer`
Manages the server side of one or more RPC services.

*   `RpcServicesServer(node_id, impl_type, config_path, ...)`: Constructor.
    *   `node_id`: The ID of the node this server is running on.
*   `bool initialize_services()`: Reads the config and initializes all services this server is responsible for.
*   `bool start_all_services()`: Starts the underlying PDU endpoints.
*   `ServerEventType poll(request_out)`: Polls for incoming requests from clients.
*   `void send_reply(header, pdu)`: Sends a response PDU back to a client. (Note: Using the service helper is recommended).

## Configuration File Schema

The service configuration is a JSON file with two main sections: `endpoints` and `services`.

```json
{
  "pduMetaDataSize": 24,
  "endpoints": [
    {
      "nodeId": "server_node",
      "endpoints": [
        { "id": "server_ep_id", "config_path": "configs/server_endpoint.json" }
      ]
    },
    {
      "nodeId": "client_node",
      "endpoints": [
        { "id": "client_ep_id", "config_path": "configs/client_endpoint.json" }
      ]
    }
  ],
  "services": [
    {
      "name": "Service/Add",
      "pdu_type_name_req": "hako_srv_msgs_AddTwoInts_req",
      "pdu_type_name_res": "hako_srv_msgs_AddTwoInts_res",
      "pdu_size_req": 16,
      "pdu_size_res": 16,
      "server_endpoint": {
        "nodeId": "server_node",
        "endpointId": "server_ep_id"
      },
      "clients": [
        {
          "name": "TestClient",
          "client_endpoint": {
            "nodeId": "client_node",
            "endpointId": "client_ep_id"
          }
        }
      ]
    }
  ]
}
```
*   **`pduMetaDataSize`**: The size of the metadata header in the PDU.
*   **`endpoints`**: An array of nodes. Each node has a `nodeId` and a list of `endpoints`, each with a unique `id` and a `config_path` to its own PDU endpoint configuration file.
*   **`services`**: An array of service definitions.
    *   `name`: The unique name of the service.
    *   `pdu_type_name_req`/`res`: The PDU type names for the request and response.
    *   `pdu_size_req`/`res`: The PDU sizes for the request and response.
    *   `server_endpoint`: Specifies which endpoint on which node acts as the server for this service.
    *   `clients`: An array of clients allowed to call this service. Each client has a unique `name` and is mapped to a specific endpoint.

## Usage Example

This example shows a simple server that provides an "Add" service and a client that calls it.

### 1. Service Definition (ROS `AddTwoInts.srv`)
```
int64 a
int64 b
---
int64 sum
```

### 2. Server Implementation (`my_server.cpp`)
```cpp
#include "hakoniwa/pdu/rpc/rpc_services_server.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoInts.hpp" // Generated from AddTwoInts.srv

int main() {
    // 1. Initialize the server for "server_node"
    hakoniwa::pdu::rpc::RpcServicesServer server(
        "server_node", "RpcServerEndpointImpl", "service_config.json", 1000);
    assert(server.initialize_services());
    assert(server.start_all_services());

    // 2. Create a helper for the AddTwoInts service
    HakoRpcServiceServerTemplateType(AddTwoInts) service_helper;

    while (true) {
        // 3. Poll for incoming requests
        hakoniwa::pdu::rpc::RpcRequest server_request;
        auto event = server.poll(server_request);

        if (event == hakoniwa::pdu::rpc::ServerEventType::REQUEST_IN) {
            // 4. Extract the typed request data
            Hako_AddTwoInts_req req_body;
            service_helper.get_request_body(server_request, req_body);

            // 5. Process the request and prepare the response
            Hako_AddTwoInts_res res_body;
            res_body.sum = req_body.a + req_body.b;

            // 6. Send the typed response back
            service_helper.reply(server, server_request, res_body);
        }
    }
    return 0;
}
```

### 3. Client Implementation (`my_client.cpp`)
```cpp
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoInts.hpp"

int main() {
    // 1. Initialize the client for "client_node" with a unique name "MyAdder"
    hakoniwa::pdu::rpc::RpcServicesClient client(
        "client_node", "MyAdder", "service_config.json", "RpcClientEndpointImpl", 1000);
    assert(client.initialize_services());
    assert(client.start_all_services());

    // 2. Create a helper for the AddTwoInts service
    HakoRpcServiceServerTemplateType(AddTwoInts) service_helper;

    // 3. Prepare the typed request
    Hako_AddTwoInts_req client_req_body;
    client_req_body.a = 5;
    client_req_body.b = 7;

    // 4. Call the service and wait for the response
    Hako_AddTwoInts_res client_res_body;
    bool result = service_helper.call(client, "Service/Add", client_req_body, client_res_body, 1000000);

    // 5. Check the result
    if (result) {
        std::cout << "Response received: " << client_res_body.sum << std::endl;
        assert(client_res_body.sum == 12);
    } else {
        std::cerr << "RPC call failed or timed out." << std::endl;
    }
    return 0;
}
```

## Architecture

The library is structured into the following main layers:

```mermaid
flowchart TB
    subgraph UserApplication["User Application"]
        direction LR
        AppClient["Client App"]
        AppServer["Server App"]
    end

    subgraph RpcServiceLayer["RPC Service Layer (This Library)"]
        RpcServicesClient["RpcServicesClient"]
        RpcServicesServer["RpcServicesServer"]
    end

    subgraph RpcEndpointLayer["RPC Endpoint Layer (This Library)"]
        RpcClientEndpoint["IRpcClientEndpoint"]
        RpcServerEndpoint["IRpcServerEndpoint"]
    end
    
    subgraph EndpointImplLayer["Endpoint Implementation"]
        RpcClientEndpointImpl["RpcClientEndpointImpl"]
        RpcServerEndpointImpl["RpcServerEndpointImpl"]
    end

    subgraph PduLayer["Hakoniwa PDU Layer"]
        PduEndpoint["hakoniwa::pdu::Endpoint"]
    end

    AppClient --> RpcServicesClient
    AppServer --> RpcServicesServer

    RpcServicesClient --> RpcClientEndpoint
    RpcServicesServer --> RpcServerEndpoint
    
    RpcClientEndpoint <--- RpcClientEndpointImpl
    RpcServerEndpoint <--- RpcServerEndpointImpl

    RpcClientEndpointImpl --> PduEndpoint
    RpcServerEndpointImpl --> PduEndpoint
```

*   **`RpcServicesServer` / `RpcServicesClient` (Service Layer)**: Manages a collection of RPC services and is the main entry point for users.
*   **`IRpcServerEndpoint` / `IRpcClientEndpoint` (Endpoint Interface Layer)**: Defines the interface for a single RPC service.
*   **`RpcServerEndpointImpl` / `RpcClientEndpointImpl` (Endpoint Implementation Layer)**: The concrete implementation that handles PDU communication.
*   **`hakoniwa::pdu::Endpoint` (PDU Layer)**: The underlying PDU transport library.

## Design Philosophy

*   **Avoid gRPC**: We chose not to use gRPC to avoid potential build and versioning complexities across different platforms and to maintain a design that is more native to the Hakoniwa ecosystem.
*   **API Consistency**: The API is designed to be consistent with existing Hakoniwa components.
*   **Decoupling Control and Data Planes**: The design encourages separating reliable control signals (a good fit for this RPC library over TCP) from high-throughput PDU data (which might use UDP or shared memory).
*   **Leverage Existing Assets**: The RPC service definitions (PDUs) are based on existing ROS IDL specifications, allowing for maximum reuse of established data structures.
