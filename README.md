# Hakoniwa PDU-RPC

`hakoniwa-pdu-rpc` is a C++ request/response layer built on `hakoniwa-pdu-endpoint`.

It is designed for Hakoniwa-native control-plane communication where explicit lifecycle, endpoint topology, and deterministic/tick-driven integration are more important than general-purpose RPC features.

## What This Library Provides

- PDU-native request/response RPC.
- Multiple named services and clients.
- JSON-driven service and endpoint topology.
- Typed request/response helpers for generated PDU service types.
- Explicit polling, timeout, and cancellation semantics.
- An installable CMake package for downstream consumers.
- Cross-platform build and contract-test coverage on Linux x64, Linux ARM64, macOS, and Windows x64.

This library intentionally does **not** provide dynamic service discovery, authentication, tracing, streaming RPC, or a hidden scheduling/threading model.

## Architecture

```text
User application
    |
    v
RpcServicesClient / RpcServicesServer
    |
    v
IRpcClientEndpoint / IRpcServerEndpoint
    |
    v
RpcClientEndpointImpl / RpcServerEndpointImpl
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

- Python 3.12 or newer for `tools/hako.py`.
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

This checks the exported CMake package and downstream link contract.

## Contract Tests

The default test command runs the reviewed RPC contract suite:

```bash
python tools/hako.py test
```

The suite is intentionally split by contract so a failure identifies which lifecycle guarantee changed.

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

## Quick Example

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

## Configuration Validation

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

## Core API

Primary application entry points:

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

Most users should work through `RpcServicesClient`, `RpcServicesServer`, and the typed helper. `IRpcClientEndpoint` / `IRpcServerEndpoint` are extension interfaces for custom endpoint behavior.

## Why Explicit Polling?

The RPC API intentionally uses `poll()` instead of imposing worker threads or a scheduler.

This lets the caller choose:

- simulation tick alignment,
- sleep/backoff policy,
- scheduling order,
- integration with an existing deterministic main loop.

The library owns RPC state transitions; the application owns scheduling policy.

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
- Examples: `examples/README.md`
- Minimal configuration: `config/sample/minimal/README.md`
- Service schema: `config/schema/service-schema.json`
