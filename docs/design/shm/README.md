# SHM RPC Endpoints (hakoniwa-core-pro services)

Status: design, 2026-11-26 (rev. 2). Tracks hakoniwalab/hakoniwa-pdu-rpc#85.

## 1. Goal

Make hakoniwa-core-pro RPC services (shared memory) reachable through
hakoniwa-pdu-rpc, in both directions, with the existing PDU-RPC API and the
existing RPC definition files:

- ROS 2 → Hakoniwa: ROS 2 users call Hakoniwa services — for example
  hakoniwa-robot-runtime's `CarService/AckermannStep/<car>`,
  `PersonService/PersonStep/<person>` and, later, robot-arm services — through
  hakoniwa-pdu-ros.
- Hakoniwa → ROS 2 (later): Hakoniwa assets call a service implemented on the
  PDU-RPC side (for example a ROS 2 service exposed by hakoniwa-pdu-ros).

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | **SHM is an endpoint protocol of PDU-RPC.** `ShmRpcServerEndpoint` (implements `IRpcServerEndpoint`) and `ShmRpcClientEndpoint` (implements `IRpcClientEndpoint`) are added to the hakoniwa-pdu-rpc library, selected by `"protocol": "shm"` in the endpoint configuration, next to `tcp` / `tcp_mux` | Applications and the RPC definition files stay as they are; only an endpoint's protocol changes |
| D2 | The SHM endpoints do **not** go through hakoniwa-pdu-endpoint | The Endpoint package is a PDU pub/sub transport. RPC semantics (request, reply, cancel, BUSY, progress) belong to core-pro's service protocol, so the SHM endpoints talk to core-pro's service API directly |
| D3 | **Dependency on core-pro by a build option**, linked dynamically in the ordinary way (`.so` / `.dylib` / `.dll`), **no `dlopen`**. CMake option `HAKO_PDU_RPC_ENABLE_SHM` (default `OFF`): `OFF` builds without core-pro and rejects protocol `shm` with a clear message; `ON` links the core-pro shared libraries | hakoniwa-pdu-rpc keeps building without core-pro by default; with SHM the OS loader resolves core-pro. A thin function table (`CoreProServiceApi`) remains only as a seam for tests (fake core-pro) |
| D4 | **A generic RPC bridge, wired by the existing RPC definition file** (section 6). The bridge connects, per service, the endpoint where it is a server to the endpoint where it is a client; both protocols come from the configuration | Like hakoniwa-pdu-bridge-core does for topics, but with RPC semantics instead of topic transfer policies (ticker / throttle / immediate do not fit RPC). One bridge covers tcp→shm (docker-host), shm→tcp (Hakoniwa → ROS), tcp→tcp |
| D5 | **One class library everywhere**: hakoniwa-pdu-ros (through `libhakoniwa_pdu_rpc`), the RPC bridge and any other PDU-RPC application use the same SHM endpoints, with the same threading and locking | One implementation, one set of tests |
| D6 | **Hakoniwa time runs on its own asset thread** (`hako_asset_usleep` loop), separated from the RPC side's real time (section 4) | A process that waits for a reply must never hold Hakoniwa time; otherwise a lockstep service deadlocks (seen in hakoniwa-robot-runtime stage P2) |
| D7 | **All core-pro calls happen on the asset thread only** | core-pro reuses one request and one response buffer per client / server; calls from two threads would race |
| D8 | Client endpoint first, server endpoint next (stages, section 9) | ROS 2 → Hakoniwa is the direction needed now |

## 3. Routes

```text
docker-only (ROS 2 and Hakoniwa on the same Linux / in one container)

  ROS 2 client -> hakoniwa-pdu-ros -> PDU-RPC client endpoint (shm)
               -> core-pro service -> server (e.g. Robot Runtime)

docker-host (ROS 2 in Docker, Hakoniwa on the host, e.g. macOS)

  [Docker] ROS 2 client -> hakoniwa-pdu-ros -> PDU-RPC client endpoint (tcp)
                                                    | TCP
  [Host]   RPC bridge:  server endpoint (tcp) -> client endpoint (shm)
                                                    -> core-pro service -> server

Hakoniwa -> ROS 2 (later)

  Hakoniwa asset (core-pro client) -> core-pro service
           -> RPC bridge: server endpoint (shm) -> client endpoint (tcp)
           -> hakoniwa-pdu-ros (PDU-RPC server) -> ROS 2 service
```

core-pro's shared memory (the mmap file) is visible only inside one OS. Docker
on macOS runs in a Linux VM, so the host's mmap is out of reach and the
docker-host route crosses the boundary over TCP.

## 4. Classes, threads and locking

```text
IRpcClientEndpoint ─┬─ RpcClientEndpointImpl     existing (tcp, tcp_mux, ... over hakoniwa-pdu-endpoint)
                    └─ ShmRpcClientEndpoint      new, protocol "shm": core-pro service client
IRpcServerEndpoint ─┬─ RpcServerEndpointImpl     existing
                    └─ ShmRpcServerEndpoint      new, protocol "shm": core-pro service server
                              │ both use
                              ▼
                        ShmRpcAsset              one per process: registers the Hakoniwa asset,
                              │                  runs the asset thread, owns the mailboxes
                              ▼
                        CoreProServiceApi        thin function table over core-pro's C API (test seam)
```

```text
RPC side (real time)                          Asset thread (Hakoniwa time, hako_asset_usleep loop)
------------------------------------          -----------------------------------------------------
client endpoint                               every Hakoniwa step:
  call(): copy request into the outbox          client side:
  poll(): read the inbox                          - queued request -> get_request_buffer, copy, call_request
  send_cancel_request(): set cancel flag          - queued cancel  -> cancel_request
server endpoint                                   - busy client    -> client_poll; RESPONSE_IN / CANCEL_DONE /
  poll(): read the inbox (requests, cancels)        TIMEOUT -> copy the reply into the inbox
  send_reply / send_cancel_reply: copy the      server side:
    reply into the outbox                         - server_poll; REQUEST_IN / REQUEST_CANCEL -> copy into the inbox
                                                  - queued reply -> get_response_buffer, copy body, put_response
                                                - advance its own time; never waits for a reply
        └──── per-endpoint mailbox: outbox + inbox, one mutex ────┘
```

- **Mailbox**: one per endpoint, one mutex. Data is copied in and out under the
  lock; nothing else is done under the lock (copy inside, convert outside).
- **The asset thread never waits for a reply.** It advances its own time every
  step and never holds world time. A lockstep service (Robot Runtime) holds the
  world time itself until its requests arrive; the asset thread then simply
  cannot run ahead of world time.
- **The RPC side never touches core-pro.** Real-time timeouts stay with the
  caller, as today.
- Start, stop and reset are read from the return values of the asset API
  (`hako_asset_usleep` and friends), following core-pro's polling samples. On
  reset, in-flight requests are dropped and reported as errors to the RPC side.

## 5. Endpoint configuration

An SHM endpoint is configured like the others; its comm file says `"protocol":
"shm"`:

```json
{
  "protocol": "shm",
  "assetName": "pdu-ros-shm",
  "deltaTimeUsec": 1000,
  "coreConfig": "/path/to/cpp_core_config.json"
}
```

- `assetName`: the Hakoniwa asset this process registers as. One asset per
  process; every SHM endpoint in the process shares it.
- `deltaTimeUsec`: the asset thread's step.
- `coreConfig`: core-pro's configuration (if not taken from the environment,
  e.g. `HAKO_CONFIG_PATH`).

Service definitions (names, types, `maxClients`, `pduSize`) come from the RPC
definition file as today (section 6). Their shape matches core-pro's
`service.json` (`name`, `type`, `maxClients`, `pduSize.server/client.baseSize/heapSize`).
Whether core-pro's `service.json` can be generated from, or replaced by, the
same file is open point O1.

## 6. RPC bridge (wired by the RPC definition file)

The bridge reads the existing RPC definition file. For each service, the
bridge's node appears once in `server_endpoints` (where callers reach it) and
once in `clients[].client_endpoint` (where it calls the real server):

```json
{
  "services": [
    {
      "name": "CarService/AckermannStep/Car-1",
      "type": "hako_srv_msgs/AckermannStep",
      "maxClients": 1,
      "pduSize": { "server": { "baseSize": 320, "heapSize": 0 },
                   "client": { "baseSize": 320, "heapSize": 0 } },
      "server_endpoints": [ { "nodeId": "rpc-bridge", "endpointId": "tcp-from-docker" } ],
      "clients": [
        { "name": "rpc-bridge-Car-1",
          "client_endpoint": { "nodeId": "rpc-bridge", "endpointId": "shm-to-core" } }
      ]
    }
  ]
}
```

`tcp-from-docker` is a tcp endpoint, `shm-to-core` an shm endpoint; swapping
them gives the reverse direction. The bridge itself knows no protocol and no
time; it only applies the forwarding rules:

| Rule | Bridge behaviour |
|---|---|
| One request in, one request out | every request received on the server side is forwarded exactly once to the client side; no throttling, merging or dropping |
| Request identity | the bridge keeps the mapping from the incoming request to the outgoing one and sends the reply back to the right caller |
| Cancel | a cancel received on the server side is forwarded; the cancel result goes back as DONE / CANCELED |
| One outstanding request per client | a second request while one is in progress is answered BUSY on the server side, not forwarded |
| Timeouts | owned by the original caller; the bridge adds none |
| Interim replies (BUSY, progress) | never end a request (hakoniwa-core-pro#123) |

## 7. Message mapping

PDU-RPC and core-pro use the same generated headers from hakoniwa-pdu-registry
(`HakoCpp_ServiceRequestHeader`, `HakoCpp_ServiceResponseHeader`), the same
status and result-code values and the same body layout, so bodies pass through
unchanged (#85, "Facts checked"). core-pro writes its own request id and client
name into the request header; the SHM endpoints keep the mapping between the
PDU-RPC request id and the Core one. Both sides must use the same
hakoniwa-pdu-registry revision.

## 8. Open points

| # | Question |
|---|---|
| O1 | Can core-pro's `service.json` be generated from (or replaced by) the RPC definition file? Today Robot Runtime generates it with `tools/gen_rpc_service_config.py` |
| O2 | The bridge executable's name and launch (one process per RPC definition file), and how it is started next to hakoniwa-pdu-bridge-core (topics) by the launcher |
| O3 | Progress (`get_progress`) and actions over SHM |

Decided on 2026-11-26 (rev. 2): asset thread = `hako_asset_usleep` loop;
core-pro linked dynamically by a build option (no `dlopen`); the bridge is a
generic RPC bridge in hakoniwa-pdu-rpc wired by the RPC definition file, not a
part of hakoniwa-pdu-bridge-core (its topic transfer policies do not fit RPC).

## 9. Stages

| Stage | Content | Done when |
|---|---|---|
| S1 | `CoreProServiceApi` (+ fake), `ShmRpcAsset`, the mailbox, `ShmRpcClientEndpoint`; `HAKO_PDU_RPC_ENABLE_SHM` | Unit tests with the fake core-pro: request, reply, cancel, timeout, one outstanding request, the asset thread never blocks on a reply; `OFF` build rejects `shm` |
| S2 | Real core-pro: a PDU-RPC client with an shm endpoint calls a Core service served by a test server asset | Integration test on Linux and macOS: OK, INVALID, CANCELED; replies byte-identical to a direct Core client |
| S3 | docker-only: hakoniwa-pdu-ros with an shm client endpoint calls Robot Runtime's `CarService/AckermannStep/<car>` (lockstep) | A ROS 2 service call returns the same reply as Robot Runtime's direct Core client test |
| S4 | Generic RPC bridge; docker-host route (tcp → shm) | hakoniwa-pdu-ros in Docker → bridge on the host → Robot Runtime: same replies as S3 |
| S5 | `ShmRpcServerEndpoint`; reverse route (shm → tcp) | A core-pro client calls a service served on the PDU-RPC side through the bridge |

Implementation and tests: Codex (Windows / WSL and macOS). Design and review:
Claude Code with Mori.
