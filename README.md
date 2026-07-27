# Hakoniwa PDU-RPC

`hakoniwa-pdu-rpc` is a C++ RPC layer for Hakoniwa built on the PDU communication model.
It provides request/response semantics that align with Hakoniwa endpoint configuration and PDU definitions.
It is intentionally narrow in scope: deterministic integration and explicit topology are prioritized over general-purpose RPC features.

## Positioning

**This is:**
- A PDU-native request/response layer for Hakoniwa execution and endpoint semantics.
- A config-driven RPC integration model that reuses Hakoniwa endpoint topology.
- A thin abstraction over `hakoniwa-pdu-endpoint` with explicit call/poll behavior.

**This is not:**
- A general-purpose RPC framework such as gRPC.
- A dynamic service mesh with runtime discovery, auth, tracing, or streaming semantics.

Design intent:
- **Explicit semantics:** request IDs, channels, timeout, and cancellation behavior remain visible.
- **Config-driven topology:** RPC wiring follows Hakoniwa node/endpoint definitions.
- **Minimal hidden assumptions:** no hidden scheduler/threading policy is imposed by default.
- **Endpoint reuse:** transport belongs to `hakoniwa-pdu-endpoint`; RPC owns request/response state.

## Links

- Build architecture: `docs/build-architecture.md`
- Examples: `examples/README.md`
- Tutorial: `docs/tutorials/rpc.md`
- Minimal config set: `config/sample/minimal/README.md`
- JSON schema: `config/schema/service-schema.json`

## Features

- **Service-oriented RPC:** define and manage multiple RPC services within a server.
- **Multi-client support:** a service can be called by multiple uniquely named clients.
- **Configuration-driven:** service/client topology is expressed in JSON.
- **Typed helper API:** `HakoRpcServiceServerTemplateType` handles PDU pack/unpack for generated service types.
- **Transport agnostic at RPC layer:** reuses Endpoint transports without changing RPC user code.
- **Core-compatible timeout/cancel semantics:** timeout is an event; cancellation is explicit and races safely with normal completion.

## Build and test

The normal entry point is `tools/hako.py`. There is intentionally no build manifest for PDU RPC because the component has one standard library/test/package shape rather than multiple product personalities.

Prerequisites:
- Python 3.12 or compatible Python 3
- CMake 3.16+
- C++20 compiler
- initialized `hakoniwa-pdu-registry` submodule
- installed `hakoniwa-pdu-endpoint` CMake package
- on Windows: Visual Studio C++ tools plus vcpkg Boost.Asio/Boost.Beast

Initialize the Registry submodule once:

```bash
git submodule update --init --recursive
```

Point to a non-default Endpoint install with either:

```bash
export HAKO_PDU_ENDPOINT_ROOT=/path/to/endpoint-install
```

or:

```bash
python tools/hako.py doctor --endpoint-root /path/to/endpoint-install
```

The standard flow is:

```bash
python tools/hako.py doctor
python tools/hako.py configure --dry-run
python tools/hako.py build
python tools/hako.py test
```

`hako.py` detects the host OS/architecture, resolves the Endpoint package, applies the Windows vcpkg toolchain when needed, and hides platform-specific CMake arguments.

The managed build enables the library, examples, and tests. The default build directory is `build`.

### Install and package contract

Install into the repository-local prefix:

```bash
python tools/hako.py install
```

Default install prefix:

```text
.hako/install
```

Validate the real installed CMake contract with a separate downstream project:

```bash
python tools/hako.py package-test
```

That flow verifies:

```text
Endpoint installed package
  -> PDU RPC build
  -> PDU RPC install
  -> external CMake consumer
  -> find_package(hakoniwa_pdu_rpc CONFIG REQUIRED)
  -> compile/link against exported targets
```

This is deliberately stronger than an in-tree build: a source-tree build passing does not prove that exported targets, installed include paths, or transitive dependency contracts are correct.

### Downstream CMake usage

New consumers should use:

```cmake
find_package(hakoniwa_pdu_rpc CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hakoniwa_pdu_rpc::rpc)
```

The historical target remains available for compatibility:

```cmake
target_link_libraries(my_app PRIVATE hakoniwa_pdu_rpc::hakoniwa_pdu_rpc)
```

PDU RPC public headers use generated service header types from the pinned `hakoniwa-pdu-registry` submodule. The install step copies those generated public header artifacts into the PDU RPC install prefix, so a package consumer does not need a source-tree Registry include path.

### Direct CMake

Direct CMake remains available for development/compatibility workflows:

```bash
cmake -S . -B build \
  -DHAKO_PDU_ENDPOINT_PREFIX=/path/to/endpoint-install \
  -DHAKO_PDU_RPC_BUILD_TESTS=ON \
  -DHAKO_PDU_RPC_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The modern dependency path prefers the installed Endpoint package target:

```text
hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint
```

A compatibility finder remains for older Endpoint installations.

## Cross-platform validation

GitHub Actions runs the same public workflow on:

- Ubuntu x64
- Ubuntu ARM64
- macOS
- Windows x64

Each job installs a Core-free Endpoint package, then runs:

```text
python tools/hako.py doctor
python tools/hako.py configure --dry-run
python tools/hako.py build
python tools/hako.py test
python tools/hako.py install
python tools/hako.py package-test
```

The tests use TCP Endpoint configuration, so the PDU RPC CI does not require Hakoniwa Core. A Core-enabled Endpoint remains usable when an application actually needs SHM/Hakoniwa runtime integration.

## Quick RPC example

After `python tools/hako.py build`, run the sample pair in two terminals.

Terminal A:

```bash
build/examples/hakoniwa_pdu_rpc_server
```

Terminal B:

```bash
build/examples/hakoniwa_pdu_rpc_client 1000000
# then type: 5 7
# expected: sum=12
```

On multi-config Windows generators, executables are normally under `build/Release/examples/`.

If the example fails, check:
- server starts before client;
- `nodeId` and client name match the service config;
- `endpointId` values match endpoint mappings;
- both sides use the same TCP port and reachable addresses.

## Configuration validation

From repository root:

```bash
export PYTHONPATH="python:$PYTHONPATH"
python -m hakoniwa_pdu_rpc.validate_configs config/sample/simple-service.json --skip-endpoint-validation
python -m hakoniwa_pdu_rpc.validate_configs test/configs/service_config.json --skip-endpoint-validation
```

Validation checks:
- service config schema;
- duplicate service/client names;
- channel collisions;
- `maxClients` bounds;
- optional Endpoint config consistency.

Notes:
- requires `jsonschema` (`pip install jsonschema`);
- Endpoint validation is provided by `hakoniwa-pdu-endpoint`;
- set `HAKO_PDU_ENDPOINT_SCHEMA` when the Endpoint schema is installed in a non-standard location;
- use `--skip-endpoint-validation` when only RPC-level validation is required.

## Timeout and cancellation contract

A timeout does not implicitly abort an RPC or immediately return the endpoint to idle.

```text
REQUEST
  -> RUNNING
  -> timeout event
  -> caller explicitly sends CANCEL
  -> CANCELLING
  -> either normal response wins
       or cancel completion wins
  -> IDLE
```

A normal response may legitimately win after timeout was observed but before cancellation reaches the server. The dedicated regression tests preserve this race behavior.

Timeout is also not a peer-failure detector. A higher layer decides what a permanently unavailable server means for its simulation/runtime.

## Core concepts

### Services

A service is a remote procedure such as `Service/Add` with request/response PDU definitions and allowed clients.

### Configuration

RPC topology is defined in service JSON. Endpoint configs are managed separately through `EndpointContainer`; service config references endpoint IDs rather than owning the transport itself.

### RPC service helper

`HakoRpcServiceServerTemplateType(AddTwoInts)` provides typed helpers such as:
- `call()`
- `get_request_body()`
- `reply()`
- `get_response_body()`
