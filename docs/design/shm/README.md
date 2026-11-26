# SHM RPC Design (hakoniwa-core-pro services)

Status: design, 2026-11-26. Tracks hakoniwalab/hakoniwa-pdu-rpc#85.

## 1. Goal

Let a hakoniwa-pdu-rpc client call a **hakoniwa-core-pro RPC service** (shared
memory), so that ROS 2 users reach Hakoniwa services — for example the
hakoniwa-robot-runtime services `CarService/AckermannStep/<car>`,
`PersonService/PersonStep/<person>` and, later, robot-arm services — through
hakoniwa-pdu-ros.

Two routes, one class library:

```text
docker-only (ROS 2 and Hakoniwa on the same Linux / in the same container)

  ROS 2 client -> hakoniwa-pdu-ros -> PDU-RPC client (protocol "shm")
                                   -> ShmRpc client -> core-pro service -> server (e.g. Robot Runtime)

docker-host (ROS 2 in Docker, Hakoniwa on the host, e.g. macOS)

  [Docker] ROS 2 client -> hakoniwa-pdu-ros -> PDU-RPC client (protocol "tcp")
                                                   | TCP
  [Host]   PDU-RPC TCP bridge = PDU-RPC server (tcp) + ShmRpc client
                                                   -> core-pro service -> server
```

core-pro's shared memory (the mmap file) is visible only inside one OS. Docker
on macOS runs in a Linux VM, so the host's mmap is out of reach and the
docker-host route crosses the boundary over TCP.

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | SHM RPC is a new **transport protocol of PDU-RPC**, selected in the runtime-loaded service configuration: `"transport": {"protocol": "shm", ...}` | Applications (hakoniwa-pdu-ros, the bridge) keep using the existing PDU-RPC API and configuration; only the protocol changes |
| D2 | It is **not** built on hakoniwa-pdu-endpoint | The Endpoint is a PDU pub/sub transport. RPC semantics (request, reply, cancel, BUSY, progress) belong to core-pro's service protocol, so SHM RPC talks to core-pro's service API directly |
| D3 | **Loose, run-time dependency on core-pro**: a thin wrapper class holds a table of core-pro C API functions (`hako_asset_*`, `hako_asset_service_*`) resolved with `dlopen` / `LoadLibrary` when protocol `shm` is used | hakoniwa-pdu-rpc keeps building without core-pro (README: "The RPC package itself does not require a direct dependency on Hakoniwa Core"). Same pattern as hakoniwa-pdu-endpoint's Core variant (`HAKO_PDU_ENDPOINT_SHARED_LIB`) |
| D4 | **One class library for both routes**: hakoniwa-pdu-ros (through `libhakoniwa_pdu_rpc` and its C API / Python binding) and the TCP bridge use the same ShmRpc classes, with the same threading and locking | One implementation, one set of tests; the bridge is only "PDU-RPC server (tcp) + ShmRpc client" glued together |
| D5 | **Hakoniwa time runs on its own thread**, separated from the RPC side's real time (section 4) | A process that waits for a reply must never hold Hakoniwa time; otherwise a lockstep service deadlocks (seen in hakoniwa-robot-runtime stage P2) |
| D6 | **All core-pro calls happen on the asset thread only** | core-pro's service client reuses one request and one response buffer per client; calls from two threads would race |
| D7 | First the **client** side (PDU-RPC client -> core-pro service). The server side (core-pro client -> PDU-RPC server, e.g. a ROS service exposed to Hakoniwa) comes later | The ROS-to-Robot-Runtime direction is the one needed now |

## 3. Classes

```text
IRpcClientEndpoint (existing interface)
  ├─ RpcClientEndpointImpl   existing: over hakoniwa-pdu-endpoint (tcp, tcp_mux, ...)
  └─ ShmRpcClientEndpoint    new: protocol "shm"
         │ owns / shares
         ▼
     ShmRpcAsset              one per process: registers the Hakoniwa asset,
         │                    runs the asset thread, owns the mailboxes
         ▼
     CoreProServiceApi        function table, filled by dlopen (D3)
```

- `ShmRpcClientEndpoint` implements `IRpcClientEndpoint` (`initialize`, `call`,
  `poll`, `send_cancel_request`, `create_request_buffer`,
  `clear_pending_responses`). It never calls core-pro; it only posts to and
  reads from its mailbox (D6).
- `ShmRpcAsset` is shared by every ShmRpc endpoint in the process: one asset
  registration, one asset thread, one Core client per (service, client name).
- `CoreProServiceApi` is a plain struct of function pointers plus `load(path)`;
  tests replace it with a fake.

## 4. Threads and locking (same in hakoniwa-pdu-ros and the bridge)

```text
RPC side (real time)                          Asset thread (Hakoniwa time)
------------------------------------          ---------------------------------------
call(): copy the request into the            every Hakoniwa step:
        endpoint's outbox, return              - for each endpoint with a queued request:
poll(): read the endpoint's inbox                get_request_buffer, copy, call_request
send_cancel_request(): set the                 - for each endpoint with a queued cancel:
        cancel flag in the outbox                cancel_request
                                               - for each busy Core client: client_poll;
                                                 on RESPONSE_IN / CANCEL_DONE / TIMEOUT,
                                                 copy the reply into the endpoint's inbox
                                               - advance its own time (never waits for a reply)
        └──── per-endpoint mailbox: outbox + inbox, one mutex ────┘
```

- **Mailbox**: one per endpoint, protected by one mutex, holding at most one
  outstanding request (core-pro allows one request per client), a cancel flag,
  and the latest reply. Data is copied in and out under the lock; nothing else
  is done under the lock (the PDU shared-memory rule: copy inside the lock,
  convert outside).
- **The asset thread never waits for a reply.** It advances its own time every
  step, so it never holds world time. A lockstep service (Robot Runtime) holds
  the world time itself until all its requests arrive; the asset thread then
  simply cannot run ahead of world time.
- **The RPC side never touches core-pro.** It may block on the mailbox (with
  its own real-time timeout, owned by the caller as today).
- The same mailbox class is used by hakoniwa-pdu-ros (via the shared library)
  and by the bridge executable.

## 5. Configuration

The service configuration already loaded at run time gets protocol `shm`:

```json
{
  "transport": {
    "protocol": "shm",
    "shm": {
      "coreLibrary": "libassets.so",
      "assetName": "pdu-ros-shm",
      "serviceConfig": "/path/to/core-service.json",
      "deltaTimeUsec": 1000
    }
  }
}
```

- `coreLibrary`: resolved like hakoniwa-pdu-endpoint's Core variant (explicit
  path, else an environment variable, else the default name). Exact lookup rule:
  open point O2.
- `assetName`: the Hakoniwa asset this process registers as.
- `serviceConfig`: core-pro's service definition (`service.json`, for Robot
  Runtime generated by `tools/gen_rpc_service_config.py`). PDU-RPC service and
  client names map to Core service and client names one to one.
- `deltaTimeUsec`: the asset thread's step.

## 6. Message mapping

PDU-RPC and core-pro use the same generated headers from hakoniwa-pdu-registry
(`HakoCpp_ServiceRequestHeader`, `HakoCpp_ServiceResponseHeader`), the same
status and result-code values, and the same body layout, so the body is copied
through unchanged (#85, "Facts checked"). core-pro writes its own request id
and client name into the request header (`get_request_buffer`); the reply's
header is core-pro's. The ShmRpc endpoint keeps the mapping from the PDU-RPC
request id to the Core request id so that the reply goes back to the right
PDU-RPC call.

Both sides must use the same hakoniwa-pdu-registry revision (service types and
sizes).

## 7. Semantics (core-pro after hakoniwa-core-pro#123)

| Event | ShmRpc client |
|---|---|
| `call()` while a request is in progress | rejected on the RPC side before anything reaches core-pro (core-pro's client also rejects it since #123) |
| Reply DONE (OK / CANCELED / ERROR) or INVALID | delivered to `poll()`; the client is free again |
| BUSY, progress (DOING) | interim: never ends the request (#123); progress may be surfaced later |
| Caller's timeout | the caller cancels (`send_cancel_request`); the asset thread calls `cancel_request`; the result arrives as DONE / CANCELED. No new request until then |
| Lockstep service | the caller sends the next request only after the reply (Robot Runtime design, section 6.2). ShmRpc enforces one outstanding request per client |

## 8. Lifecycle

- The asset registers at the first `initialize` of a `shm` endpoint and starts
  the asset thread; it follows the simulation (start / stop / reset) as a
  normal asset. On reset, in-flight requests are dropped and the RPC side gets
  an error for them.
- On process exit the asset thread stops and the asset unregisters.

## 9. Open points

| # | Question |
|---|---|
| O1 | The asset thread's model: core-pro callback (`hako_asset_start` with `on_simulation_step`) or polling (`hako_asset_usleep` loop). Callback matches hakopy and the samples |
| O2 | Library lookup order for `coreLibrary` (path / environment variable / default name), aligned with hakoniwa-pdu-endpoint's Core variant |
| O3 | The bridge executable: name, its configuration (one TCP server config + one SHM client config per service), and whether it lives in this repository |
| O4 | Progress (`get_progress`) and actions over SHM: later |
| O5 | SHM server side (D7): later |

## 10. Stages

| Stage | Content | Done when |
|---|---|---|
| S1 | `CoreProServiceApi` + fake; `ShmRpcAsset` and the mailbox; `ShmRpcClientEndpoint` behind protocol `shm` | Unit tests with the fake Core: request, reply, cancel, timeout, one-outstanding rule, asset thread never blocks on a reply |
| S2 | Real core-pro: a PDU-RPC client with protocol `shm` calls a Core service served by a test server asset | Integration test on Linux and macOS: OK, INVALID, CANCELED; replies byte-identical to a direct Core client |
| S3 | docker-only route: hakoniwa-pdu-ros with protocol `shm` calls Robot Runtime's `CarService/AckermannStep/<car>` (lockstep) | ROS 2 service call returns the same reply as the Core client test in Robot Runtime |
| S4 | TCP bridge (PDU-RPC server tcp + ShmRpc client) and the docker-host route | hakoniwa-pdu-ros in Docker -> bridge on the host -> Robot Runtime: same replies as S3 |

Implementation and tests: Codex (Windows / WSL and macOS). Design and review:
Claude Code with Mori.
