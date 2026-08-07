# Hakoniwa PDU-RPC

`hakoniwa-pdu-rpc` is a C++ request/response and long-running Action layer built on `hakoniwa-pdu-endpoint`. Service RPC supports typed synchronous and asynchronous Python clients. Action supports point-to-point Client/Server and a TCP Mux Server through native C++, C, and Python CFFI APIs.

It is designed for Hakoniwa-native control-plane communication where explicit lifecycle, endpoint topology, and deterministic/tick-driven integration are more important than general-purpose RPC features.

## What This Library Provides

- PDU-native request/response RPC.
- Multiple named services and clients.
- JSON-driven service and endpoint topology.
- Typed request/response helpers for generated PDU service types.
- Native C++ and C APIs for point-to-point and TCP Mux Action Goal lifecycles.
- Python CFFI APIs for Service clients and Action Client/Server/Mux Server.
- ROS-independent synchronous/asynchronous Service APIs and explicitly polled Action APIs.
- Explicit polling, timeout, and cancellation semantics.
- An installable CMake package for downstream consumers.
- Cross-platform build and contract-test coverage on Linux x64, Linux ARM64, macOS, and Windows x64.

This library intentionally does **not** provide dynamic service discovery, authentication, tracing, streaming RPC, or a general-purpose scheduler. The C++ API remains explicitly polled. The Python high-level `call_async()` API uses one background worker per in-flight call while remaining independent of ROS and `asyncio`.

## Architecture

```text
User application
    |                                      |
    v                                      v
RpcServicesClient / Server        ActionServicesClient / Server
                                           |
                                           +-- ActionServicesMuxServer
    |                                      |
    v                                      v
RPC Endpoint contract              Action Endpoint contract
    |                                      |
    +------------------+-------------------+
                       |
                       v
              hakoniwa-pdu-endpoint
    |
    v
configured PDU transport
```

`hakoniwa-pdu-rpc` depends on the public CMake target provided by `hakoniwa-pdu-endpoint`:

```text
hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint
```

The RPC package itself does not require a direct dependency on Hakoniwa Core. Whether a particular endpoint configuration needs additional runtime components is an Endpoint concern.

## RPC Lifecycle Contract

The client lifecycle is intentionally explicit.

Normal request:

```text
IDLE
  -> call()
RUNNING
  -> RESPONSE_IN
IDLE
```

A finite timeout is a **notification**, not automatic cancellation:

```text
RUNNING
  -> RESPONSE_TIMEOUT
RUNNING
  -> caller send_cancel_request()
CANCELLING
  -> RESPONSE_CANCEL
IDLE
```

`timeout == 0` means no RPC timeout deadline. The request remains in flight until a response is received.

A normal response may also win after cancellation has been requested:

```text
RUNNING
  -> RESPONSE_TIMEOUT
  -> send_cancel_request()
CANCELLING
  -> normal RESPONSE_IN wins before cancel completion
IDLE
```

A late server-side CANCEL must be harmless. This race is part of the reviewed RPC contract and is covered by the contract-test suite.

Server loss is not treated as a separate RPC recovery protocol here. Applications that require server-death detection or high-availability recovery should provide that policy above this layer.

See `docs/test-contract.md` for the detailed test ownership and lifecycle rationale.

## Action Lifecycle Contract

An Action is a Goal lifecycle identified by a caller-supplied, non-zero
128-bit `goal_id`. It is not modeled as a long Service call.

```text
Client                         Server
  |-- Goal Request ------------>|
  |<-- Goal Response -----------|  ACCEPTED or REJECTED
  |<-- Feedback (0..n) ---------|
  |-- Cancel Request ---------->|  optional
  |<-- Cancel Response ---------|
  |<-- Result ------------------|  SUCCEEDED, CANCELED, or ABORTED
```

The Runtime correlates packets, owns Goal state transitions, rejects active
`goal_id` collisions, and preserves response ordering. The Application owns
Goal ID generation, whether a Goal is accepted, its execution policy, Feedback
contents, and terminal Result contents.

The native, C, and Python APIs are explicitly polled. The initial Action
implementation does not create a Future/callback scheduler. For a TCP Mux
Server, connection identity remains internal: the Application still addresses
a Goal with `action_name + ServerGoalHandle`. `is_ready()` becomes true only
after all expected transport sessions have been adopted as initialized Action
connection slots.

The complete protocol and state contracts are indexed in
[`docs/design/action/README.md`](docs/design/action/README.md).

## Python Service Client API

Set the Python package path and shared-library path after building the native library:

```bash
export PYTHONPATH="$PWD/python:$PYTHONPATH"
export HAKO_PDU_RPC_LIBRARY=/path/to/libhakoniwa_pdu_rpc.so
```

The high-level Python `RpcClient` exposes both synchronous and asynchronous calls. The synchronous API is implemented on top of the same asynchronous lifecycle implementation, so timeout, cancellation, and response/cancel race handling are not duplicated.

```python
from hakoniwa_pdu_rpc import RpcClient

client = RpcClient(
    library_path="/path/to/libhakoniwa_pdu_rpc.so",
    node_id="client-node",
    client_name="client1",
    service_config_path="config/sample/simple-service.json",
    endpoint_config_path="config/sample/endpoints.json",
)
client.start()

response_pdu = client.call(
    "Service/Add",
    request_pdu,
    timeout_usec=1_000_000,
)
```

For executor-based integrations, use `call_async()` and observe the returned `RpcFuture`:

```python
future = client.call_async(
    "Service/Add",
    request_pdu,
    timeout_usec=1_000_000,
)

future.add_done_callback(lambda completed: print(completed.result()))
```

`RpcFuture` provides:

- `result()`
- `exception()`
- `done()`
- `running()`
- `add_done_callback()`
- protocol-level `cancel()`

The Future is independent of `rclpy` and `asyncio`. An adapter such as `hakoniwa-pdu-ros` can forward completion into its own executor without blocking the caller thread.

The native client currently owns one shared response queue. Therefore, one Python `RpcClient` instance permits one in-flight request at a time. Use separate client instances for independent concurrent requests.

### Registry-generated typed service auto-wiring

`TypedRpcClient` loads request/response packet classes and converters by service naming convention. When no package is specified, discovery prefers the installed `hakoniwa-pdu` layout and falls back to the Registry source-tree layout:

```text
hakoniwa_pdu.pdu_msgs.hako_srv_msgs
pdu.python.hako_srv_msgs
```

```python
from hakoniwa_pdu_rpc import make_typed_client

add_client = make_typed_client(
    client,
    service_name="Service/Add",
    service_type="AddTwoInts",
)

request = add_client.create_request()
request.body.a = 5
request.body.b = 7
response = add_client.call(request, timeout_usec=1_000_000)
```

The auto-wire layer owns packet conversion and normalizes generated converter output to Python `bytes`. Higher-level bridges should map only the service request/response body and leave packet headers, request IDs, timeout, and cancellation lifecycle to PDU-RPC.

## Repository Setup

Clone the repository including the generated PDU registry submodule:

```bash
git clone --recursive https://github.com/hakoniwalab/hakoniwa-pdu-rpc.git
cd hakoniwa-pdu-rpc
```

For an existing checkout:

```bash
git submodule update --init --recursive
```

## Dependencies

Required:

- Python 3.12 or newer for `tools/hako.py` and the Python binding.
- C++20 compiler.
- CMake 3.16 or newer.
- Installed `hakoniwa-pdu-endpoint` CMake package.
- `hakoniwa-pdu-registry` submodule from this repository.

`nlohmann_json` is resolved by the project and is included in the installed package contract.

On Windows, the build driver uses vcpkg when required by Endpoint dependencies.

## Recommended Build Interface: `tools/hako.py`

The repository provides one cross-platform build driver so callers do not need to reproduce platform-specific CMake details. Its user-facing build intent is defined by the repository-root [`hakoniwa-build.yaml`](hakoniwa-build.yaml).

```bash
python tools/hako.py doctor
python tools/hako.py build
```

`doctor` checks the host platform, CMake/Git availability, Registry submodule, Endpoint installation, and Windows vcpkg prerequisites.

### Build manifest

The default manifest reproduces the behavior that predates manifest support:

```yaml
version: 1

build:
  type: Release
  dir: build
  install_dir: .hako/install

paths:
  pdu_endpoint_root: ""
  vcpkg_root: ""
```

| Key | Default | Meaning |
|---|---|---|
| `version` | `1` | Manifest schema version |
| `build.type` | `Release` | CMake build configuration |
| `build.dir` | `build` | Build directory, relative to the repository when not absolute |
| `build.install_dir` | `.hako/install` | Local install prefix used by `hako.py` |
| `paths.pdu_endpoint_root` | `""` | Installed Endpoint package root; empty enables environment/automatic discovery |
| `paths.vcpkg_root` | `""` | Windows vcpkg root; empty enables environment/automatic discovery |

All keys are required in schema v1. Unknown keys, missing keys, unsupported build types, and invalid value types are rejected before CMake runs.
Relative `build.*` and manifest `paths.*` values are resolved from the repository root. Relative paths supplied explicitly through CLI options retain the existing current-directory-relative behavior.

When `--config` is omitted, `hako.py` always uses the repository-root `hakoniwa-build.yaml`, regardless of the current directory. An explicitly supplied relative path is resolved from the current directory:

```bash
python tools/hako.py doctor --config test/fixtures/alternate-build.yaml
python tools/hako.py build --config test/fixtures/alternate-build.yaml
```

The selected manifest is only an input layer. The existing operation semantics remain unchanged:

```text
build
  -> tests OFF
  -> examples ON

test / test-*
  -> tests ON
  -> reviewed test targets and CTest

package-test
  -> build
  -> install
  -> external package consumer validation
```

Resolved configuration is written to `.hako/resolved-build.yaml`, and the operation-specific CMake arguments are written to `.hako/cmake-args.txt`. Both are generated files.

### Overrides and dependency discovery

Existing CLI overrides remain available:

```bash
python tools/hako.py build \
  --build-dir out/build \
  --install-dir out/install \
  --build-type Debug \
  --endpoint-root /path/to/endpoint/install
```

Build directory, install directory, and build type use:

```text
explicit CLI option > selected manifest value
```

Endpoint and vcpkg roots use:

```text
explicit CLI option
  > selected manifest value
  > existing environment variables
  > existing automatic discovery
```

A non-empty CLI or manifest dependency path is authoritative. If the selected path is invalid, `doctor` reports it instead of silently replacing it with a lower-priority environment or automatically discovered path.

The Endpoint prefix environment variables remain:

```bash
export HAKO_PDU_ENDPOINT_ROOT=/path/to/endpoint/install
# or
export HAKO_PDU_ENDPOINT_PREFIX=/path/to/endpoint/install
```

On Windows, vcpkg can still be supplied through `VCPKG_ROOT` or `VCPKG_INSTALLATION_ROOT`.

### Operations

The common workflow is:

```bash
python tools/hako.py doctor
python tools/hako.py build
python tools/hako.py test
python tools/hako.py install
python tools/hako.py package-test
```

`install` keeps the native/CMake-only behavior by default. To also install the
`hakoniwa_pdu_rpc` Python package into a Foundation-owned virtual environment,
select that environment explicitly:

```bash
python tools/hako.py install \
  --install-dir /path/to/foundation/install \
  --endpoint-root /path/to/foundation/install \
  --python-venv /path/to/foundation/install/python
```

The virtual environment must already exist. With `--python-venv`, the
Component Receipt records the Python package and enables the
`python_rpc_mux_server`, `typed_async_client`, `shared_native_library`, and
`python_package` capabilities. Without it, the Python capabilities remain
disabled and the existing native install contract is preserved.
Python build and runtime dependencies are resolved from `pyproject.toml` by
`pip`. A clean environment therefore works without preinstalling CFFI,
jsonschema, or setuptools, but the initial install may require package-index
access when those dependencies are not already cached.

### Inspect the generated CMake command

```bash
python tools/hako.py configure --dry-run
```

This is useful when integrating the package into another build environment without duplicating the repository's platform policy.

## Direct CMake Usage

Direct CMake is also supported.

```bash
cmake -S . -B build \
  -DHAKO_PDU_ENDPOINT_PREFIX=/path/to/endpoint/install \
  -DHAKO_PDU_RPC_BUILD_TESTS=ON \
  -DHAKO_PDU_RPC_BUILD_EXAMPLES=ON

cmake --build build --parallel
```

The preferred dependency is an installed Endpoint CMake package. `HAKO_PDU_ENDPOINT_PREFIX` is retained as a hint/compatibility path for Endpoint installations.

On Unix-like systems, the historical direct-CMake install default remains `/usr/local/hakoniwa`. `tools/hako.py install` uses an explicit repository-local prefix instead.

## Install and Downstream CMake Usage

Install with the build driver:

```bash
python tools/hako.py install
```

The installed package exports:

```text
hakoniwa_pdu_rpc::rpc
hakoniwa_pdu_rpc::rpc_shared
```

Downstream CMake:

```cmake
find_package(hakoniwa_pdu_rpc REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hakoniwa_pdu_rpc::rpc)
```

The compatibility target below is also provided:

```cmake
hakoniwa_pdu_rpc::hakoniwa_pdu_rpc
```

Use `hakoniwa_pdu_rpc::rpc` for the static native contract and
`hakoniwa_pdu_rpc::rpc_shared` when a shared-library boundary is required,
including CFFI consumers.

For a non-standard install prefix:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/path/to/rpc/install;/path/to/endpoint/install"
```

### Package contract verification

`package-test` verifies the installed package from a separate consumer project rather than relying on the in-tree build:

```bash
python tools/hako.py package-test
```

This checks the exported CMake package from an out-of-tree consumer. The
consumer compiles and links the Service RPC and Action Mux C++ APIs against
both `hakoniwa_pdu_rpc::rpc` and `hakoniwa_pdu_rpc::rpc_shared`, compiles the
Action Mux C API against both targets, and includes a Registry-generated Action
header from the install tree. When `--python-venv` is supplied, it also imports
the installed `ActionMuxServer` from an isolated Python process.

## Contract Tests

The default test command runs the reviewed Service RPC and Action contract suite:

```bash
python tools/hako.py test
```

The suite is intentionally split by contract so a failure identifies which lifecycle guarantee changed.

The Action portion covers configuration resolution, Client and Server state
reducers, packet codecs, Services Goal lifecycle, and a real TCP round trip for
Goal Response, Feedback, Cancel, and Result. It is included in `hako.py test`.
The runnable Fibonacci Action pair is documented in
[`examples/README.md`](examples/README.md). Focused Service RPC commands remain
available as shown below.

| Contract | Command | Main guarantee |
|---|---|---|
| Basic | `test-basic` | round trip and endpoint reuse |
| Infinite wait | `test-infinite-wait` | `timeout == 0` never emits RPC timeout |
| Timeout/cancel | `test-timeout-cancel` | timeout -> explicit cancel -> terminal cancel -> reuse |
| Cancel race | `test-cancel-race` | normal response may win while client is cancelling |

Examples:

```bash
python tools/hako.py test-basic
python tools/hako.py test-infinite-wait
python tools/hako.py test-timeout-cancel
python tools/hako.py test-cancel-race
```

The contract tests are also exercised in CI on:

- Ubuntu x64
- Ubuntu ARM64
- macOS
- Windows x64

A test failure is not automatically a production bug. Before changing RPC implementation, verify that the failing test still represents the documented lifecycle contract. See `docs/test-contract.md`.

## Quick Examples

### Service RPC

The build driver enables examples during normal builds:

```bash
python tools/hako.py build
```

Run server and client in separate terminals.

Terminal A:

```bash
build/examples/hakoniwa_pdu_rpc_server
```

Terminal B:

```bash
build/examples/hakoniwa_pdu_rpc_client 1000000
```

Then enter:

```text
5 7
```

Expected result:

```text
sum=12
```

Reference files:

- `examples/rpc_server.cpp`
- `examples/rpc_client.cpp`
- `examples/README.md`
- `config/sample/simple-service.json`
- `config/sample/endpoints.json`
- `config/sample/minimal/`

### Fibonacci Action

Generate the point-to-point TCP runtime configuration from the user-facing
Action manifest:

```bash
python tools/generate_action_config.py \
  --config config/sample/action.json \
  --output .hako/action
```

Then run the server and client in separate terminals:

```bash
build/examples/hakoniwa_pdu_action_fibonacci_server
```

```bash
build/examples/hakoniwa_pdu_action_fibonacci_client 10
```

The expected lifecycle is Goal acceptance, zero or more Feedback packets, and
a terminal Fibonacci Result. Stop the server with `Ctrl+C`. See
[`examples/README.md`](examples/README.md) for generated paths, explicit path
arguments, and troubleshooting.

## Configuration

### Service RPC configuration

Service topology is defined in JSON and can be validated before runtime.

From the repository root:

```bash
export PYTHONPATH="python:$PYTHONPATH"
python -m hakoniwa_pdu_rpc.validate_configs \
  config/sample/simple-service.json \
  --skip-endpoint-validation
```

The validator checks RPC schema and consistency, including required fields, duplicate names, channel collisions, and `maxClients` constraints.

Endpoint-side validation can also be used when the Endpoint validator/schema is available.

Common configuration mistakes include:

- `nodeId` mismatch between code and service configuration.
- client-name mismatch.
- `endpointId` mismatch with Endpoint configuration.
- request/response channel collisions.
- incorrect generated PDU sizes.

Schema:

```text
config/schema/service-schema.json
```

### Action configuration

[`config/sample/action.json`](config/sample/action.json) is the user-facing
manifest for the initial point-to-point TCP generator. Users specify:

- Action name and generated type in `package/ActionName` form;
- `slotCount`;
- Client and Server `nodeId` values;
- TCP roles and addresses;
- optional request, response, and Feedback heap capacities.

`bufferHeap.requestSize`, `responseSize`, and `feedbackSize` default to 1 MiB
when omitted, and the generator emits a warning. They are capacity limits, not
wire sizes: encoded packets are sent at their actual size, and an encoded body
that exceeds its configured capacity is rejected.

```bash
python tools/generate_action_config.py \
  --config config/sample/action.json \
  --output .hako/action
```

The generator validates the manifest and atomically writes:

```text
.hako/action/resolved-action.json
.hako/action/endpoints.json
.hako/action/queue.json
.hako/action/endpoints/*.json
.hako/action/transport/*.json
```

Channel IDs, channel names, packet types, Endpoint IDs, queue configuration,
and EndpointContainer entries are generated details. Applications should not
duplicate them manually. This generator currently defines the point-to-point
TCP deployment. A TCP Mux Server consumes its explicitly supplied Mux Endpoint
configuration; Mux topology is not silently inferred from this manifest.

## Core API

Primary native application entry points:

### `RpcServicesClient`

```text
RpcServicesClient(node_id, client_name, config_path, ...)
initialize_services(endpoint_container)
start_all_services()
call(service_name, request_pdu, timeout_usec)
poll(service_name, response_out)
send_cancel_request(service_name)
```

### `RpcServicesServer`

```text
RpcServicesServer(node_id, impl_type, config_path, ...)
initialize_services(endpoint_container, ...)
start_all_services()
poll(request_out)
send_reply(...)
```

### Typed service helper

`HakoRpcServiceServerTemplateType(...)` provides typed conversion helpers such as:

- `call()`
- `get_request_body()`
- `reply()`
- `get_response_body()`

Most native users should work through `RpcServicesClient`, `RpcServicesServer`, and the typed helper. `IRpcClientEndpoint` / `IRpcServerEndpoint` are extension interfaces for custom endpoint behavior.

### Native Action API

The primary point-to-point entry points are `ActionServicesClient` and
`ActionServicesServer`. `ActionServicesMuxServer` owns multiple TCP sessions
while preserving the same Application-facing Server Goal API.

```text
ActionServicesClient
  initialize_services() / start_all_services()
  create_goal_buffer()
  send_goal_with_result()
  poll()
  send_cancel()

ActionServicesServer
  initialize_services() / start_all_services()
  poll()
  accept_goal() / reject_goal()
  accept_cancel() / reject_cancel()
  create_feedback_buffer() / send_feedback()
  create_result_buffer() / complete()

ActionServicesMuxServer
  initialize() / start() / stop()
  poll() and the same Goal response/Feedback/Result operations
  connected_count() / expected_count() / is_ready()
```

Buffers are allocated before encoding because generated PDU converters require
a writable capacity. Applications encode into those buffers; send operations
validate and transmit the encoded packet rather than allocating a replacement
packet internally.

## Action C and Python APIs

The point-to-point Action C API is declared in:

```c
#include <hakoniwa/pdu/action/c_action.h>
```

It is a thin C boundary over the native Action Services. The C layer owns the
opaque Client/Server handle, buffer allocation, type conversion, and exception
containment; it does not duplicate the native Goal state machine. A Goal is
identified consistently by `action_name` and a typed Client or Server Goal
handle.

The basic lifecycle is:

```text
create -> start -> wait for is_running -> send/poll -> stop -> destroy
```

`start()` starts asynchronous Endpoint processing. It does not guarantee that
the TCP peer is connected, so a sender must observe `is_running != 0` before
the first Goal. Buffers returned by a `*_alloc()` function belong to the caller
and must be released with `hako_pdu_action_buffer_free()`.

The C API supports point-to-point Action Client/Server operation and a TCP Mux
Action Server. The Python package exposes the same contract through
`ActionClient`, `ActionServer`, and `ActionMuxServer`:

```python
import time

from hakoniwa_pdu_rpc import ActionClient, ActionClientEvent

client = ActionClient(
    library_path,
    "fibonacci-client",
    "my-action-client",
    action_config_path,
    endpoint_config_path,
)
client.start()
while not client.is_running():
    time.sleep(0.001)

goal_pdu = client.create_goal_buffer("fibonacci")
goal = client.send_goal("fibonacci", goal_pdu, goal_id_bytes)
event = client.poll()
```

Python Goal IDs are exactly 16 bytes and must not be all-zero. Native error numbers are exposed
as `ActionErrorCode`, and `ActionError.code` preserves the precise synchronous
failure reason. The Python layer copies allocated native buffers into `bytes`
and releases the native allocation before returning.

Both C and Python Action APIs remain explicitly polled and share the Native
Goal state machine. `ActionMuxServer` keeps connection identity and routing
internal; callers continue to use `action_name + ServerGoalHandle`. Higher-level
Future/callback adapters remain a separate follow-up layer. See
[`docs/design/action/09-c-api.md`](docs/design/action/09-c-api.md) for the full
contract.

Service RPC, RPC Mux, and Action register their C declarations in one CFFI
binding and load the same `libhakoniwa_pdu_rpc` shared library. The binding is
cached by normalized library path, so they share one `FFI` and one `dlopen()`
result within a process. No Action-specific shared library is introduced.

## Scheduling Model

The native C++ RPC API intentionally uses `poll()` instead of imposing worker threads or a scheduler. This lets the caller choose simulation tick alignment, sleep/backoff policy, scheduling order, and integration with an existing deterministic main loop.

The Python high-level Service `call_async()` adapter makes a different tradeoff: it drives the same RPC lifecycle state machine in a daemon worker and exposes completion through `RpcFuture`. This is suitable for ROS executors, GUIs, and other callback-oriented applications. The initial Python Action CFFI API remains explicitly polled; a Future/callback Action adapter is not implied by the Service implementation.

In both cases, the RPC implementation owns state transitions. The selected application adapter owns how completion is integrated into its execution model.

## Why Not gRPC?

This project is not intended to replace gRPC.

Use PDU-RPC when Hakoniwa-native topology, generated PDU assets, deterministic integration, and a small middleware surface are the important constraints.

Use a general-purpose RPC framework when you need capabilities such as:

- broad polyglot client support,
- streaming,
- authentication/authorization,
- tracing and service observability,
- dynamic service infrastructure.

A hybrid architecture is also possible, but it intentionally introduces two middleware stacks and two lifecycle/debugging models.

## Documentation

- Test contract and audit policy: `docs/test-contract.md`
- RPC tutorial: `docs/tutorials/rpc.md`
- Action design index: `docs/design/action/README.md`
- Action configuration contract: `docs/design/action/08-configuration.md`
- Action C API contract: `docs/design/action/09-c-api.md`
- Action Mux Server contract: `docs/design/action/13-mux-server.md`
- Examples: `examples/README.md`
- Minimal configuration: `config/sample/minimal/README.md`
- Service schema: `config/schema/service-schema.json`
