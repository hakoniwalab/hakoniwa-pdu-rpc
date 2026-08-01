# Python Binding Design

## Status

Proposed design for exposing `hakoniwa-pdu-rpc` to Python through a stable C API and CFFI.

This document defines the boundary and ownership model before implementation. It does not define the final Python package layout or every public symbol name.

## Goals

The Python binding shall:

- reuse the existing C++ RPC implementation as the single source of truth;
- expose both RPC client and RPC server capabilities;
- use a stable C ABI suitable for CFFI;
- preserve the existing explicit `call` / `poll` / `cancel` lifecycle;
- preserve the existing server `poll` / `reply` lifecycle;
- work consistently on Linux, macOS, and Windows;
- hide C++ object ownership and `EndpointContainer` construction from Python users;
- avoid C++-thread-to-Python callbacks at the CFFI boundary;
- keep request and response payloads as raw PDU byte buffers at the ABI boundary;
- allow higher-level Python wrappers to provide Pythonic APIs without duplicating the RPC state machine.

## Non-goals

The first implementation does not attempt to:

- reimplement the RPC protocol or state machine in Python;
- expose C++ classes directly to Python;
- expose generated C++ PDU types through the ABI;
- provide asynchronous framework integration directly in the C API;
- add service discovery, streaming, authentication, or tracing;
- change the existing RPC timeout and cancellation contract;
- make the Python binding responsible for generating endpoint or service topology.

## Why CFFI

Hakoniwa uses CFFI as the preferred Python interoperability boundary where portability matters.

Compared with binding C++ classes directly, a C ABI provides:

- a smaller and more stable binary contract;
- less coupling to a particular Python version;
- less coupling to a particular C++ ABI;
- a consistent integration pattern across Linux, macOS, and Windows;
- a reusable boundary for languages other than Python;
- alignment with the existing `hakoniwa-pdu-endpoint` Python integration.

The C++ implementation remains authoritative. Python is a consumer of that implementation, not a second implementation of it.

## Existing C++ API Selected as the Basis

The C API shall be based on the application-level classes:

- `hakoniwa::pdu::rpc::RpcServicesClient`
- `hakoniwa::pdu::rpc::RpcServicesServer`

It shall not be based directly on the lower-level extension interfaces:

- `IRpcClientEndpoint`
- `IRpcServerEndpoint`

The application-level classes already own the intended RPC lifecycle and support multiple named services.

### Client lifecycle

The existing client API provides:

```text
initialize_services(endpoint_container)
start_all_services()
call(service_name, request_pdu, timeout_usec)
poll(service_name, response_out)
send_cancel_request(service_name)
stop_all_services()
clear_all_instances()
```

### Server lifecycle

The existing server API provides:

```text
initialize_services(endpoint_container)
start_all_services()
poll(request_out)
send_reply(header, pdu)
send_cancel_reply(header, pdu)
stop_all_services()
clear_all_instances()
```

These APIs are already polling-oriented. Therefore, the first CFFI design does not require an application callback from C or C++ into Python.

## Architecture

```text
Python application
    |
    v
Python wrapper
    |
    v
CFFI-generated module
    |
    v
Stable C API
    |
    +-- owns EndpointContainer
    +-- owns RpcServicesClient or RpcServicesServer
    |
    v
Existing C++ PDU-RPC implementation
    |
    v
hakoniwa-pdu-endpoint
```

The C API is an ownership and representation adapter. It must not duplicate RPC behavior.

## Configuration Model

The RPC runtime still requires:

- an endpoint configuration file;
- a service configuration file;
- node and client identity values;
- runtime parameters such as delta time and time-source type.

However, Python users shall not construct or pass a C++ `EndpointContainer` object.

The C API wrapper shall create and own the runtime objects internally:

```text
C API create
    -> construct EndpointContainer
    -> initialize EndpointContainer
    -> construct RpcServicesClient or RpcServicesServer
    -> initialize RPC services using the EndpointContainer

C API start
    -> start EndpointContainer
    -> start RPC services

C API stop/destroy
    -> stop RPC services
    -> stop EndpointContainer
    -> release all C++ objects
```

The configuration files remain explicit inputs. Their generation or authoring may be handled by repository tooling or higher-level products, but object construction and initialization are internal to the binding.

## Ownership Model

Each C handle owns the complete runtime required by that role.

Conceptually:

```cpp
struct HakoRpcClientHandle {
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoints;
    std::unique_ptr<hakoniwa::pdu::rpc::RpcServicesClient> rpc;
};

struct HakoRpcServerHandle {
    std::shared_ptr<hakoniwa::pdu::EndpointContainer> endpoints;
    std::unique_ptr<hakoniwa::pdu::rpc::RpcServicesServer> rpc;
};
```

The actual types remain private to the C++ implementation.

The public header exposes opaque handles only:

```c
typedef struct hako_rpc_client hako_rpc_client_t;
typedef struct hako_rpc_server hako_rpc_server_t;
```

### Lifecycle rules

- A handle returned by `create` must be released by the matching `destroy` function.
- `destroy` must be safe after a failed or partial initialization.
- `stop` should be idempotent.
- `destroy` should perform any required stop operation before releasing resources.
- A handle must not be used after `destroy`.
- The first implementation need not support concurrent calls on the same handle unless explicitly documented.

## Callback Policy

The public C API shall not require C-to-Python callbacks for normal RPC operation.

This is possible because the existing RPC API is explicitly polling-oriented:

- the client calls `poll` to observe response, timeout, and cancellation events;
- the server calls `poll` to observe requests and cancellation requests.

Avoiding callbacks at the ABI boundary prevents common CFFI hazards:

- Python GIL ownership from native worker threads;
- callback object lifetime and garbage collection;
- Python exceptions crossing a C callback boundary;
- shutdown races between callback execution and handle destruction;
- platform-specific callback and calling-convention differences.

Internal C++ callbacks used by `hakoniwa-pdu-endpoint` remain an implementation detail. They are not directly exposed through this binding.

If future requirements need connection notifications, the preferred C API extension is another polling state or status query, not a mandatory native callback into Python.

## Event Model

The C API shall preserve the current RPC event semantics.

### Client events

```c
typedef enum hako_rpc_client_event {
    HAKO_RPC_CLIENT_EVENT_NONE = 0,
    HAKO_RPC_CLIENT_EVENT_RESPONSE_IN,
    HAKO_RPC_CLIENT_EVENT_RESPONSE_CANCEL,
    HAKO_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT
} hako_rpc_client_event_t;
```

Transport or API failures are function-result errors, not additional RPC protocol events, unless implementation work demonstrates that a distinct event is required.

### Server events

```c
typedef enum hako_rpc_server_event {
    HAKO_RPC_SERVER_EVENT_NONE = 0,
    HAKO_RPC_SERVER_EVENT_REQUEST_IN,
    HAKO_RPC_SERVER_EVENT_REQUEST_CANCEL
} hako_rpc_server_event_t;
```

The enum values shall be explicitly assigned and treated as ABI-stable after release.

## Error Model

C functions shall return a binding-level result code separate from RPC events.

Example shape:

```c
typedef enum hako_rpc_result {
    HAKO_RPC_RESULT_OK = 0,
    HAKO_RPC_RESULT_INVALID_ARGUMENT,
    HAKO_RPC_RESULT_INVALID_STATE,
    HAKO_RPC_RESULT_NOT_FOUND,
    HAKO_RPC_RESULT_BUFFER_TOO_SMALL,
    HAKO_RPC_RESULT_CONFIG_ERROR,
    HAKO_RPC_RESULT_ENDPOINT_ERROR,
    HAKO_RPC_RESULT_RPC_ERROR,
    HAKO_RPC_RESULT_INTERNAL_ERROR
} hako_rpc_result_t;
```

The exact mapping shall be finalized during implementation.

The C API should also expose the last diagnostic message per handle or per thread without returning pointers to temporary C++ strings. One possible API is:

```c
size_t hako_rpc_client_get_last_error(
    const hako_rpc_client_t* client,
    char* buffer,
    size_t capacity);
```

The function returns the required length, including or excluding the terminating null byte according to one documented convention.

## Buffer and Memory Contract

The ABI boundary shall use caller-owned buffers.

The binding must not require Python to free memory allocated by the C++ library.

Request input:

```c
const uint8_t* request_data;
size_t request_size;
```

Response or request output:

```c
uint8_t* output_buffer;
size_t output_capacity;
size_t* output_size;
```

### Buffer-too-small behavior

When the provided buffer is too small:

- no partial payload shall be treated as valid;
- the function shall return `HAKO_RPC_RESULT_BUFFER_TOO_SMALL`;
- `output_size` shall contain the required payload size;
- the RPC event or request shall remain retrievable until the caller successfully reads it, or the implementation shall use a documented two-phase query/read contract.

The implementation must not silently discard an event merely because the first Python-provided buffer was too small.

A two-phase contract may be used if it better matches the existing C++ objects:

```text
poll metadata and required size
    -> allocate Python buffer
    -> copy current event payload
    -> consume event
```

The final choice shall be covered by C API tests.

## Header Representation

Generated C++ service header types must not cross the C ABI directly.

The C API shall define plain C structures containing only stable fixed-width fields and bounded or caller-copied strings.

Conceptual request metadata:

```c
typedef struct hako_rpc_request_info {
    uint64_t request_id;
    uint8_t opcode;
    /* service name and client name are copied through explicit buffers
       or represented by documented fixed limits */
} hako_rpc_request_info_t;
```

Conceptual response metadata:

```c
typedef struct hako_rpc_response_info {
    uint64_t request_id;
    uint8_t status;
    int32_t result_code;
} hako_rpc_response_info_t;
```

The actual fields shall be derived from the existing generated request and response headers. No C++ `std::string`, `std::vector`, or generated C++ type may appear in the public C header.

## Proposed Client API Shape

The following is an initial shape, not yet a frozen ABI.

```c
hako_rpc_result_t hako_rpc_client_create(
    const char* node_id,
    const char* client_name,
    const char* endpoint_config_path,
    const char* service_config_path,
    const char* implementation_type,
    uint64_t delta_time_usec,
    const char* time_source_type,
    hako_rpc_client_t** out_client);

hako_rpc_result_t hako_rpc_client_start(
    hako_rpc_client_t* client);

hako_rpc_result_t hako_rpc_client_call(
    hako_rpc_client_t* client,
    const char* service_name,
    const uint8_t* request_data,
    size_t request_size,
    uint64_t timeout_usec);

hako_rpc_result_t hako_rpc_client_poll(
    hako_rpc_client_t* client,
    hako_rpc_client_event_t* out_event,
    char* service_name_buffer,
    size_t service_name_capacity,
    size_t* service_name_size,
    hako_rpc_response_info_t* out_info,
    uint8_t* response_buffer,
    size_t response_capacity,
    size_t* response_size);

hako_rpc_result_t hako_rpc_client_cancel(
    hako_rpc_client_t* client,
    const char* service_name);

hako_rpc_result_t hako_rpc_client_stop(
    hako_rpc_client_t* client);

void hako_rpc_client_destroy(
    hako_rpc_client_t* client);
```

The binding shall preserve the existing contract:

```text
call
    -> repeated poll
    -> RESPONSE_IN or RESPONSE_TIMEOUT

RESPONSE_TIMEOUT
    -> caller explicitly calls cancel
    -> repeated poll
    -> RESPONSE_CANCEL or a normal RESPONSE_IN wins the race
```

The C API must not introduce automatic cancellation unless a separate higher-level Python convenience method explicitly implements that policy.

## Proposed Server API Shape

```c
hako_rpc_result_t hako_rpc_server_create(
    const char* node_id,
    const char* endpoint_config_path,
    const char* service_config_path,
    const char* implementation_type,
    uint64_t delta_time_usec,
    const char* time_source_type,
    hako_rpc_server_t** out_server);

hako_rpc_result_t hako_rpc_server_start(
    hako_rpc_server_t* server);

hako_rpc_result_t hako_rpc_server_poll(
    hako_rpc_server_t* server,
    hako_rpc_server_event_t* out_event,
    hako_rpc_request_info_t* out_info,
    char* service_name_buffer,
    size_t service_name_capacity,
    size_t* service_name_size,
    char* client_name_buffer,
    size_t client_name_capacity,
    size_t* client_name_size,
    uint8_t* request_buffer,
    size_t request_capacity,
    size_t* request_size);

hako_rpc_result_t hako_rpc_server_reply(
    hako_rpc_server_t* server,
    const hako_rpc_request_info_t* request_info,
    const char* service_name,
    const char* client_name,
    uint8_t status,
    int32_t result_code,
    const uint8_t* response_data,
    size_t response_size);

hako_rpc_result_t hako_rpc_server_cancel_reply(
    hako_rpc_server_t* server,
    const hako_rpc_request_info_t* request_info,
    const char* service_name,
    const char* client_name,
    uint8_t status,
    int32_t result_code,
    const uint8_t* response_data,
    size_t response_size);

hako_rpc_result_t hako_rpc_server_stop(
    hako_rpc_server_t* server);

void hako_rpc_server_destroy(
    hako_rpc_server_t* server);
```

The implementation may use an opaque request token instead of reconstructing the generated request header from copied fields. If used, the token must have a clearly defined lifetime and must not contain a raw Python-owned pointer.

## Python Wrapper Layer

The low-level CFFI module should be wrapped by Python classes.

Conceptual client API:

```python
with RpcClient(config) as client:
    client.call("Service/Add", request_bytes, timeout_usec=1_000_000)

    while True:
        event = client.poll()
        if event.type is ClientEvent.RESPONSE_IN:
            response_bytes = event.payload
            break
        if event.type is ClientEvent.RESPONSE_TIMEOUT:
            client.cancel("Service/Add")
```

Conceptual server API:

```python
with RpcServer(config) as server:
    while True:
        event = server.poll()
        if event.type is ServerEvent.REQUEST_IN:
            response = handler(event.payload)
            server.reply(event.request, response)
```

A later convenience method may provide synchronous policy:

```python
response = client.call_and_wait(
    "Service/Add",
    request_bytes,
    timeout_usec=1_000_000,
    cancel_on_timeout=True,
)
```

That method belongs to the Python wrapper. It must be implemented using the same low-level C API lifecycle and must not change the underlying C++ contract.

## Threading and Scheduling

The existing RPC library intentionally leaves scheduling to the application. The Python binding shall preserve that property.

- `poll` is non-blocking unless a separate wait API is explicitly added.
- The Python caller chooses sleep, backoff, simulation tick alignment, or executor integration.
- The first release should document whether one handle may be used from multiple Python threads.
- If the implementation is not internally serialized, the Python wrapper should protect each handle with a lock or document single-threaded use.
- CFFI calls that may block for a meaningful duration must be reviewed for GIL behavior.

A timeout-aware `wait` function may be added later, but it is not required to make the initial binding usable.

## Build and Package Direction

The implementation should follow the existing Endpoint CFFI pattern where practical:

```text
include/hakoniwa/pdu/rpc/c_api.h
src/c_api.cpp
python/hakoniwa_pdu_rpc/
python/hakoniwa_pdu_rpc/build_ffi.py
```

Exact paths may be adjusted to match repository conventions.

The shared library and generated CFFI extension must be tested on:

- Linux x64;
- Linux ARM64;
- macOS;
- Windows x64.

The C API symbols shall use an export macro suitable for shared-library builds on all supported platforms.

## Validation Strategy

The binding must be validated at three layers.

### C API tests

Test the C API directly before CFFI:

- create/start/stop/destroy;
- invalid configuration;
- normal request/response;
- multiple named services;
- timeout notification;
- explicit cancellation;
- response-wins-cancel race;
- buffer-too-small retry;
- repeated handle reuse;
- partial initialization cleanup.

### CFFI unit tests

Test:

- argument conversion;
- bytes input and output;
- enum mapping;
- exception mapping;
- object lifetime;
- context-manager cleanup;
- error-message retrieval.

### Cross-language contract tests

At minimum:

```text
C++ client  -> C++ server
Python client -> C++ server
C++ client  -> Python server
Python client -> Python server
```

All combinations must preserve the existing RPC lifecycle contract.

## Initial Implementation Order

1. Add the stable C header and opaque-handle implementation.
2. Implement common create/start/stop/destroy ownership.
3. Implement client call/poll/cancel.
4. Add direct C API client tests against the existing C++ server.
5. Implement server poll/reply/cancel-reply.
6. Add direct C API server tests against the existing C++ client.
7. Add the CFFI build module.
8. Add low-level Python wrappers.
9. Add Python-to-C++ and C++-to-Python contract tests.
10. Add Python-to-Python contract tests.
11. Integrate the client binding into `hakoniwa-pdu-ros` Service support.

## Decisions Recorded

This design records the following decisions:

1. The existing C++ PDU-RPC implementation remains the single source of truth.
2. Python support is provided through a C API and CFFI.
3. Both client and server roles are included in the binding scope.
4. `RpcServicesClient` and `RpcServicesServer` are the C API basis.
5. The C wrapper owns `EndpointContainer` construction, startup, shutdown, and destruction.
6. Python users provide configuration paths and identities, not C++ runtime objects.
7. The ABI uses opaque handles, fixed-width C types, and caller-owned buffers.
8. The CFFI boundary uses explicit polling rather than native callbacks into Python.
9. Timeout remains a notification; cancellation remains an explicit caller action.
10. Higher-level synchronous or asynchronous convenience behavior belongs in the Python wrapper, not the C++ RPC core.

## Open Implementation Questions

The following details remain to be finalized from code-level implementation work:

- the exact request identifier fields available in generated RPC headers;
- whether server replies should use copied header fields or an opaque request token;
- the best buffer-too-small retention mechanism;
- exact error-code mapping from Endpoint and RPC failures;
- whether connection state should be exposed as a separate query;
- whether the first C API should expose `create_request_buffer` and `create_reply_buffer` directly;
- Python package and shared-library discovery rules;
- whether the CFFI module links at build time or loads the shared library dynamically.

These questions do not change the architectural decisions above.