# Service RPC Design

This document is the canonical design index for generated Service
configuration and the Python Typed Service boundary.

## Scope

Service RPC has two independent contracts:

1. a direction-neutral manifest is resolved into native PDU-RPC topology;
2. generated Request/Response packets are exposed to Python applications as
   typed bodies without exposing packet headers or raw buffers.

The generator does not decide which application acts as the Service Client or
Server. It emits both roles from one resolved model.

## Responsibility Boundary

```text
Upstream adapter or application
  - domain-specific type discovery
  - packet base-size discovery
  - semantic request/response heap limits
  - logical client/server node IDs
             |
             v
hakoniwa-pdu-rpc Service generator
  - client identities
  - per-Service channel allocation
  - native pduSize placement
  - Endpoint IDs and mappings
  - queue and PDU definitions
  - TCP client / tcp_mux server topology
             |
             v
Generated native configuration
  - rpc-server-services.json
  - rpc-client-services.json
  - endpoints.json and endpoints/*.json
  - queue.json and pdudef.json
  - transport/*.json
```

An adapter such as `hakoniwa-pdu-ros` may resolve ROS and generated PDU types,
then write the abstract manifest. It must not reproduce channel, Endpoint, or
transport generation rules.

## User-facing Manifest

```json
{
  "version": 1,
  "services": [
    {
      "name": "Service/Add",
      "type": "hako_srv_msgs/AddTwoInts",
      "maxClients": 4,
      "clientNamePrefix": "hakoniwa_pdu_ros_add",
      "packetSize": {
        "requestBaseSize": 296,
        "responseBaseSize": 288
      },
      "bufferHeap": {
        "requestSize": 0,
        "responseSize": 0
      },
      "clientEndpoint": {"nodeId": "client_node"},
      "serverEndpoint": {"nodeId": "server_node"}
    }
  ],
  "transport": {
    "protocol": "tcp",
    "packetVersion": "v2",
    "queueDepth": 256,
    "endpoints": {
      "client_node": {
        "role": "client",
        "remote": {"address": "127.0.0.1", "port": 50001}
      },
      "server_node": {
        "role": "server",
        "local": {"address": "0.0.0.0", "port": 50001}
      }
    }
  }
}
```

The manifest is strict. Missing fields, unknown fields, duplicate Service
names, invalid endpoint roles, and unresolved endpoint references are rejected
before files are written.

### Allocation rules

- Client names are `<clientNamePrefix>_<index>`.
- Channel IDs are scoped by Service and begin at zero.
- Client index `n` uses request channel `2*n` and response channel `2*n+1`.
- Request base size maps to native `pduSize.server.baseSize`; response base
  size maps to `pduSize.client.baseSize`, following the existing native
  sender/receiver allocation contract.
- Semantic request heap maps to `pduSize.client.heapSize`; semantic response
  heap maps to `pduSize.server.heapSize`.
- A server endpoint becomes `tcp_mux`; `expected_clients` is the sum of
  `maxClients` for Services using that endpoint.

Generation is deterministic, idempotent, and atomic. The resolved manifest is
written alongside native output so failures can be explained without treating
file hashes as the primary contract.

## Python Typed Runtime

`TypedRpcClient` and `TypedRpcServer` are adapters over the raw CFFI runtime.
They own generated packet lookup and conversion, not RPC state transitions.

```text
TypedRpcClient
  typed Request body -> generated Request packet -> RpcClient
  RpcClient response -> generated Response packet -> typed Response body

TypedRpcServer
  RpcServer/RpcMuxServer event -> generated Request packet -> typed Request body
  typed Response body -> generated Response packet -> original request token
```

One `TypedRpcServer` loads every Service in a Service config. `poll()` returns
the Service name, client identity, native request token, event kind, and typed
request body. The application selects the already-resolved Service adapter with
`service(event.service_name)`.

The public typed boundary exposes:

- `create_response()`;
- `send_reply()`;
- `send_error()`;
- `send_cancel_reply()`.

It does not expose Service headers, packet metadata, channel IDs, Endpoint
routing, or raw buffers.

## Error Contract

A normal response uses `DONE/OK`. Typed server errors use the existing Service
Response Header and a non-OK result code. `TypedRpcClient` raises
`TypedRpcServiceError` and preserves the Service name, status, and result code;
it never returns a default response body as a successful result.

Cancel acknowledgement uses `DONE/CANCELED` through `send_cancel_reply()`.
The Typed layer does not invent another timeout/cancel state machine.

## Concurrency and Lifecycle

- One raw `RpcClient` supports one in-flight request. Independent concurrent
  calls require independent clients.
- `RpcMuxServer` accepts independent client transports and preserves native
  request-token routing.
- `TypedRpcServer` does not own transport start/stop; the raw server remains
  the lifecycle authority.
- An async client future publishes terminal completion only after its in-flight
  lock is released, allowing a pool to lease the client from a done callback.
- `timeout_usec=0` means no native RPC deadline. An owning bridge may implement
  one higher-level deadline and request normal protocol cancellation once.

## Public Interfaces

Generator:

```bash
python tools/generate_service_config.py \
  --config hakoniwa-service.json \
  --output .hako/service
```

Installed command:

```text
hako-pdu-rpc-generate-service-config
```

Python runtime:

```python
from hakoniwa_pdu_rpc import make_typed_client, make_typed_server
```

The generated topology is an implementation artifact. The abstract manifest
and the typed APIs are the user-facing contracts.
