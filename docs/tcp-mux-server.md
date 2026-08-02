# TCP Multiplexed RPC Server

## Purpose

The normal PDU-RPC server owns a static `EndpointContainer`. A normal TCP
server endpoint accepts one active transport session at a time, so changing its
communication JSON alone does not create a multi-client RPC server.

`RpcServicesMuxServer` owns the complete server-side multiplexing lifecycle:

```text
multiple RpcClients
        |
        v
one TCP listener / EndpointCommMultiplexer
        |
        +-- accepted Endpoint session 1 -- RpcServicesServer adapter 1
        +-- accepted Endpoint session 2 -- RpcServicesServer adapter 2
        `-- accepted Endpoint session N -- RpcServicesServer adapter N
```

The listener address and port remain shared. The operating system distinguishes
TCP connections by their source and destination address/port tuple.

## Configuration

The mux server receives an Endpoint mux configuration directly. The endpoint
configuration contains the normal PDU definition and cache references, plus a
communication configuration with `expected_clients`.

```json
{
  "name": "rpc_mux_server",
  "pdu_def_path": "rpc-pdudef.json",
  "cache": "rpc-queue.json",
  "comm": "rpc-server-tcp-mux.json"
}
```

```json
{
  "protocol": "tcp",
  "direction": "inout",
  "local": {
    "address": "0.0.0.0",
    "port": 54010
  },
  "expected_clients": 4
}
```

`protocol` remains `tcp`: `EndpointCommMultiplexer` selects the TCP multiplexer
implementation and turns each accepted socket into an opened Endpoint.

Every client uses a normal TCP client endpoint and connects to the same server
address and port.

## C++ API

```cpp
#include "hakoniwa/pdu/rpc/rpc_services_mux_server.hpp"

hakoniwa::pdu::rpc::RpcServicesMuxServer server(
    "server_node",
    "RpcServerEndpointImpl",
    "rpc-server-services.json",
    "rpc-server-endpoint-mux.json",
    1000,
    "real");

if (!server.initialize() || !server.start()) {
    // startup error
}

hakoniwa::pdu::rpc::RpcMuxRequest request;
const auto event = server.poll(request);
if (event == hakoniwa::pdu::rpc::ServerEventType::REQUEST_IN) {
    // Build the response, then route it to the exact accepted session.
    server.send_reply(request, response_pdu);
}
```

`poll()` also accepts new sessions and removes disconnected slots. The returned
`connection_id` is opaque and must be retained with the request until reply or
cancel completion.

The existing static `RpcServicesServer` API remains unchanged.

## C API

The C facade uses a separate handle so existing applications remain source and
binary compatible:

```c
hako_pdu_rpc_mux_server_handle_t* server =
    hako_pdu_rpc_mux_server_create(
        "server_node",
        "rpc-server-services.json",
        "rpc-server-endpoint-mux.json",
        1000,
        "real");

hako_pdu_rpc_mux_server_start(server);
```

The request token returned by `hako_pdu_rpc_mux_server_poll()` contains the
accepted-session identity internally. Callers use the token with the mux reply
functions; they do not manage connection IDs directly.

Allocation-capable variants are provided for language bindings:

- `hako_pdu_rpc_mux_server_poll_alloc()`
- `hako_pdu_rpc_mux_server_create_reply_buffer_alloc()`

Buffers returned by these functions are released with
`hako_pdu_rpc_buffer_free()`.

## Lifecycle ownership

The mux server owns:

- the TCP listener;
- accepted Endpoint sessions;
- one RPC adapter per accepted session;
- disconnect callbacks and slot cleanup;
- request-token to session routing.

Applications own the service operation logic and must continue polling the
server. Disconnect cleanup and acceptance of newly connected clients occur at
that polling boundary.
