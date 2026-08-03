# Action Implementation Policy

## 1. Purpose

This document fixes the physical layout, naming, build integration, and Python/CFFI integration policy for implementing the Action runtime defined in the preceding design documents.

The implementation should preserve the structural symmetry with the existing RPC Service runtime while keeping Action-specific protocol and lifecycle logic in dedicated classes.

The main policy is:

```text
Native implementation:
  separate Action classes and headers

Build and distribution:
  integrate into the existing hakoniwa-pdu-rpc library/package

Python/CFFI foundation:
  integrate into the existing hakoniwa_pdu_rpc package and cffi_api.py

High-level Python API:
  use Action-specific modules
```

This avoids introducing another native library, shared object, Python distribution, or CFFI build path solely for Action support.

## 2. Reference structure

The existing RPC Service runtime is the implementation reference.

```text
RpcServicesClient              ActionServicesClient
RpcServicesServer              ActionServicesServer
IRpcClientEndpoint             IActionClientEndpoint
IRpcServerEndpoint             IActionServerEndpoint
RpcClientEndpointImpl          ActionClientEndpointImpl
RpcServerEndpointImpl          ActionServerEndpointImpl
RpcServicesMuxServer           ActionServicesMuxServer
```

The Action implementation should follow the same layer boundaries:

```text
Application / Adapter
  -> Services management layer
  -> Endpoint interface
  -> Endpoint implementation
  -> hakoniwa-pdu-endpoint
```

Only the internal protocol runtime differs. Action-specific state, Goal Contexts, Feedback, Result delivery, and cancellation lifecycle must not be added to the existing RPC Service state machines.

## 3. C++ namespace

Action types and implementations use a dedicated namespace:

```cpp
namespace hakoniwa::pdu::action {
}
```

They must not be placed in `hakoniwa::pdu::rpc` merely because they are built by the same package.

The package and library remain named `hakoniwa-pdu-rpc` for compatibility and distribution simplicity. The namespace expresses the public protocol concept; the package name expresses the existing delivery unit.

## 4. Public header layout

Public C++ headers are placed under:

```text
include/hakoniwa/pdu/action/
```

Initial expected files are:

```text
include/hakoniwa/pdu/action/
├── action_types.hpp
├── action_client_endpoint.hpp
├── action_server_endpoint.hpp
├── action_client_endpoint_impl.hpp
├── action_server_endpoint_impl.hpp
├── action_services_client.hpp
├── action_services_server.hpp
├── action_services_mux_server.hpp
└── c_action.h
```

### 4.1 Header responsibilities

`action_types.hpp`

- shared Action event types
- Goal identifiers and token-related native types
- request, response, feedback, and result wrapper types
- Client and Server lifecycle states where they are part of the native contract

`action_client_endpoint.hpp`

- `IActionClientEndpoint`
- one Action Type runtime interface from the Client side

`action_server_endpoint.hpp`

- `IActionServerEndpoint`
- one Action Type runtime interface from the Server side

`action_client_endpoint_impl.hpp`

- `ActionClientEndpointImpl`
- Goal Context Registry and Client protocol implementation

`action_server_endpoint_impl.hpp`

- `ActionServerEndpointImpl`
- Goal Context Registry and Server protocol implementation

`action_services_client.hpp`

- `ActionServicesClient`
- configuration loading and Action Type lookup
- application-facing Client aggregation API

`action_services_server.hpp`

- `ActionServicesServer`
- configuration loading and Action Type lookup
- application-facing Server aggregation API

`action_services_mux_server.hpp`

- `ActionServicesMuxServer`
- transport-session management corresponding to `RpcServicesMuxServer`

`c_action.h`

- public C ABI for Action Client, Server, and Mux Server
- opaque handles, events, information structures, token types, and buffer ownership contract

### 4.2 Public versus private definitions

A type belongs in a public header only when it is required by:

- a downstream C++ application,
- the C ABI implementation,
- a test that validates the public contract, or
- another installed Hakoniwa component.

Internal queue entries, registry map nodes, transport callback helpers, and implementation-only state should remain private to the implementation classes or source files.

## 5. Native source layout

Native implementation files are placed in the existing `src/` directory:

```text
src/
├── action_services_client.cpp
├── action_services_server.cpp
├── action_services_mux_server.cpp
├── action_client_endpoint_impl.cpp
├── action_server_endpoint_impl.cpp
├── c_action.cpp
├── c_action_alloc.cpp
└── c_action_mux.cpp
```

The names intentionally mirror the existing RPC sources.

### 5.1 Source responsibilities

`action_services_client.cpp`

- construct and manage Action Client Endpoint instances
- route Client operations by Action name
- aggregate polling across configured Action Types

`action_services_server.cpp`

- construct and manage Action Server Endpoint instances
- aggregate Goal and Cancel events
- route Goal Context operations to the owning endpoint

`action_services_mux_server.cpp`

- accept transport sessions
- expose the same application-level Action Server contract as the static server
- preserve Goal lifecycle independently from transport-session cleanup as required by the architecture

`action_client_endpoint_impl.cpp`

- register Action PDU definitions with Endpoint
- receive Action Response and Feedback PDUs
- manage multiple Client Goal Contexts
- dispatch Goal Response, Feedback, Cancel Response, and Result events

`action_server_endpoint_impl.cpp`

- register Action PDU definitions with Endpoint
- receive Action Request PDUs
- manage Server Goal Contexts
- dispatch Goal and Cancel requests
- send Goal Response, Cancel Response, Feedback, and Result PDUs

`c_action.cpp`

- static Client and Server C ABI
- opaque handle ownership
- event-token and goal-token maps
- conversion between native events and C structures

`c_action_alloc.cpp`

- `*_alloc()` convenience functions
- Action buffer allocation helpers
- shared release through the existing package-level buffer-free contract where practical

`c_action_mux.cpp`

- Mux Server C ABI
- connection identity hidden behind the same token model exposed by the static Server API

## 6. Naming policy

### 6.1 Native C++ names

Use the Action concept explicitly:

```text
ActionServicesClient
ActionServicesServer
ActionServicesMuxServer
IActionClientEndpoint
IActionServerEndpoint
ActionClientEndpointImpl
ActionServerEndpointImpl
```

Do not use names such as `RpcActionClient` or `RpcActionServer`. Action shares the package and transport foundation with RPC Service, but it is a separate application protocol.

### 6.2 C ABI prefix

Use:

```text
hako_pdu_action_*
```

Examples:

```c
hako_pdu_action_client_create(...)
hako_pdu_action_client_send_goal(...)
hako_pdu_action_client_cancel_goal(...)
hako_pdu_action_client_poll(...)

hako_pdu_action_server_poll(...)
hako_pdu_action_server_accept_goal(...)
hako_pdu_action_server_reject_goal(...)
hako_pdu_action_server_send_feedback(...)
hako_pdu_action_server_complete(...)
```

The exact function signatures follow `08-c-api.md` and should be implemented without changing its lifecycle model during coding.

## 7. Build contract

Action support is part of the default `hakoniwa-pdu-rpc` build.

No build option such as the following should be introduced:

```text
HAKO_PDU_RPC_ENABLE_ACTION
HAKO_PDU_ACTION_BUILD
```

The Action sources are added to the existing source list and built into both existing native artifacts:

```text
hakoniwa_pdu_rpc              static library
hakoniwa_pdu_rpc_shared       shared library
```

The existing exported targets remain unchanged:

```text
hakoniwa_pdu_rpc::rpc
hakoniwa_pdu_rpc::rpc_shared
hakoniwa_pdu_rpc::hakoniwa_pdu_rpc
```

No separate target such as `hakoniwa_pdu_action::action` is introduced in the initial implementation.

### 7.1 Shared library name

The shared-library runtime name remains:

```text
libhakoniwa_pdu_rpc.so
libhakoniwa_pdu_rpc.dylib
hakoniwa_pdu_rpc.dll
```

The same shared library exports both Service RPC and Action C APIs.

### 7.2 Installation

The existing recursive installation of `include/` installs the Action headers. The existing package configuration and downstream linking contract remain valid.

A downstream application continues to use:

```cmake
find_package(hakoniwa_pdu_rpc REQUIRED)
target_link_libraries(app PRIVATE hakoniwa_pdu_rpc::rpc)
```

No additional `find_package()` call is required for Action support.

## 8. Python package policy

Action support is added to the existing Python distribution:

```text
hakoniwa_pdu_rpc
```

Do not create a separate distribution or import root such as:

```text
hakoniwa_pdu_action
```

The native library, error handling, buffer ownership, packaging, and installation path are already shared. A separate package would duplicate operational concerns without providing a meaningful isolation boundary.

Expected package layout:

```text
python/hakoniwa_pdu_rpc/
├── cffi_api.py
├── client.py
├── server.py
├── future.py
├── action_client.py
├── action_server.py
└── ... existing modules
```

Additional small Action-specific support modules may be introduced where they clarify responsibilities, but the import root and distribution remain unchanged.

## 9. CFFI integration policy

The low-level Action CFFI wrapper is integrated into the existing:

```text
python/hakoniwa_pdu_rpc/cffi_api.py
```

It should not introduce a separate shared-library loader or parallel FFI initialization path.

`cffi_api.py` should expose low-level wrappers corresponding to the C ABI:

```text
RpcClient
RpcServer
RpcMuxServer
ActionClient
ActionServer
ActionMuxServer
```

### 9.1 Shared CFFI responsibilities

The Action wrapper reuses the existing mechanisms for:

- locating and loading `libhakoniwa_pdu_rpc`,
- `ffi.cdef()` registration,
- native error conversion,
- opaque handle lifetime,
- buffer copying,
- native buffer release,
- close-state validation.

### 9.2 Buffer ownership

The existing ownership rule remains:

```text
Native allocated buffer
  -> copy immediately to Python bytes
  -> release native buffer immediately
```

Native buffer pointers must not escape the low-level CFFI wrapper.

### 9.3 Type mapping

Use the following Python representations:

```text
goal_id       -> bytes of the protocol-defined fixed length
event_token   -> int
goal_token    -> int
PDU payload   -> bytes
Action event  -> enum plus an immutable result object
```

The wrapper must validate Goal ID length at the CFFI boundary rather than relying on the native implementation to read arbitrary Python buffers safely.

### 9.4 Close behavior

`close()` remains idempotent. Opaque native handles are owned by their Python wrapper and destroyed exactly once.

## 10. High-level Python API

High-level Action behavior is separated into Action-specific modules even though the CFFI foundation and distribution are shared.

```text
client.py          RpcClient
action_client.py   ActionClient and ActionGoalHandle

server.py          RpcServer
action_server.py   ActionServer and application-facing Goal Context
```

This distinction is required because Service RPC and Action have different user-facing lifecycles.

### 10.1 Action Client high-level responsibility

`ActionClient` may provide:

```text
send_goal_async()
ActionGoalHandle
feedback dispatch
cancel operation
result Future
```

The high-level layer owns worker threads, Future completion, callback dispatch, and polling orchestration.

The low-level CFFI layer remains callback-free and executor-independent.

### 10.2 hakoniwa-pdu-ros integration

`hakoniwa-pdu-ros` is the primary initial consumer of the Action Client API.

The integration boundary is:

```text
hakoniwa-pdu-rpc ActionClient
  -> protocol events and Python Futures

hakoniwa-pdu-ros
  -> ROS Goal Handle, feedback callback, cancel response, and result completion
```

ROS-specific types, executors, and callbacks must not be introduced into `hakoniwa-pdu-rpc`.

### 10.3 Action Server high-level responsibility

`ActionServer` should expose an application-oriented API corresponding to:

```text
poll Goal or Cancel event
accept or reject Goal
accept or reject Cancel
send Feedback
complete with terminal Result
```

It should hide Action packet headers and transport/session identity from the application.

## 11. Future policy

The existing `future.py` may be extended with generic primitives that are genuinely shared. Action-specific Goal, Cancel, and Result lifecycle behavior should not be forced into `RpcFuture` if that obscures semantics.

Either of the following is acceptable:

```text
future.py
  RpcFuture
  shared completion primitive

action_client.py
  ActionResultFuture / ActionGoalHandle behavior
```

or:

```text
future.py
  RpcFuture
  ActionResultFuture
  ActionCancelFuture
```

The implementation should choose the smallest structure that keeps the Action lifecycle explicit. It must not create another Python distribution merely to separate Future classes.

## 12. Testing placement

Action tests remain in the existing repository test structure.

Recommended organization:

```text
test/action/
├── test_action_basic.cpp
├── test_action_feedback.cpp
├── test_action_cancel.cpp
├── test_action_result.cpp
├── test_action_multi_goal.cpp
└── test_action_mux.cpp
```

Python tests remain under the existing Python test conventions and import from `hakoniwa_pdu_rpc`.

Contract tests should be added to the default reviewed test suite. Action support is not an optional feature, so its baseline tests must run in the normal build and CI contract.

## 13. Initial implementation sequence

Codex or another implementation agent should implement the design incrementally in this order:

1. Add public Action types and endpoint interfaces.
2. Add `ActionServicesClient` and `ActionServicesServer` skeletons.
3. Add Client Goal Context Registry and basic Goal/Result path.
4. Add Server Goal Context Registry and Goal acceptance path.
5. Add Feedback.
6. Add Client and runtime cancellation paths.
7. Add the C ABI and token maps.
8. Add Mux support while preserving Goal lifetime rules.
9. Integrate Action declarations and wrappers into `cffi_api.py`.
10. Add high-level `action_client.py` and `action_server.py`.
11. Add contract tests and a FibonacciAction end-to-end example.
12. Integrate the resulting Action Client into `hakoniwa-pdu-ros` in a separate repository change.

Each implementation PR should preserve compilability and testability. Large generated or mechanical changes should not be combined with unresolved protocol design changes.

## 14. Decisions fixed by this document

The initial implementation uses the following decisions:

1. Action uses `hakoniwa::pdu::action`.
2. Public headers use `include/hakoniwa/pdu/action/`.
3. Native sources remain in the existing `src/` directory.
4. The C ABI is declared in `c_action.h` with the `hako_pdu_action_*` prefix.
5. Action is built by default into the existing static and shared libraries.
6. Existing CMake exported targets remain unchanged.
7. The existing shared-library filename remains unchanged.
8. The Python distribution remains `hakoniwa_pdu_rpc`.
9. Low-level Action CFFI is integrated into the existing `cffi_api.py`.
10. High-level Action Client and Server APIs use Action-specific Python modules.
11. ROS-specific behavior remains in `hakoniwa-pdu-ros`.
12. Action baseline tests run as part of the default build and CI contract.

These decisions are implementation policy. A coding agent should not introduce alternative package boundaries, build options, target names, or shared-library names without a separate reviewed design change.
