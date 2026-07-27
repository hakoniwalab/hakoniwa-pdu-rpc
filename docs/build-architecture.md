# Hakoniwa PDU RPC Build Architecture

## Goal

`hakoniwa-pdu-rpc` is a small request/response layer above `hakoniwa-pdu-endpoint`.
Its build contract should therefore stay simpler than components that have multiple
runtime personalities: there is no build manifest and no direct Hakoniwa Core
selection in PDU RPC.

The supported user-facing flow is:

```text
python tools/hako.py doctor
python tools/hako.py configure --dry-run
python tools/hako.py build
python tools/hako.py test
python tools/hako.py install
python tools/hako.py package-test
```

`tools/hako.py` owns host/architecture detection, Endpoint package discovery,
Windows vcpkg resolution, CMake arguments, local install layout, and the external
package-consumer check.

## Dependency boundary

```text
hakoniwa-pdu-endpoint installed CMake package
        |
        | hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint
        v
hakoniwa-pdu-rpc
        |
        | install / export
        v
hakoniwa_pdu_rpc::rpc
        |
        v
external CMake consumer
```

PDU RPC does not reconstruct Endpoint's transitive Boost/Core/nlohmann dependency
paths. The Endpoint package owns its dependency contract.

A compatibility `FindHakoniwaPduEndpoint.cmake` path remains for older Endpoint
installations, but it normalizes the result to the same namespaced Endpoint target.

## Public generated headers

PDU RPC public headers include generated service headers and `pdu_convertor.hpp`
from the pinned `hakoniwa-pdu-registry` submodule. Those generated headers are
therefore part of the installed PDU RPC header contract.

The Registry repository remains the source of truth for generated PDU types; the
PDU RPC install step copies the pinned generated header artifacts needed by
installed downstream consumers so they do not depend on a source-tree include
path.

## Exported targets

New consumers use:

```cmake
find_package(hakoniwa_pdu_rpc CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hakoniwa_pdu_rpc::rpc)
```

The historical target name remains available for compatibility:

```cmake
target_link_libraries(my_app PRIVATE hakoniwa_pdu_rpc::hakoniwa_pdu_rpc)
```

## Package contract test

`python tools/hako.py package-test` validates a real install boundary:

```text
Endpoint package already installed
  -> configure/build PDU RPC
  -> install PDU RPC to .hako/install
  -> configure test/package_consumer as a separate CMake project
  -> find_package(hakoniwa_pdu_rpc CONFIG REQUIRED)
  -> build consumers against both new and compatibility targets
```

This is intentionally separate from an in-tree build. A source-tree build passing
is not evidence that exported include paths, dependency targets, or installed
headers are correct.

## Cross-platform CI

The cross-platform workflow validates the same public `hako.py` vocabulary on:

- Ubuntu x64
- Ubuntu ARM64
- macOS
- Windows x64

Each job first installs a Core-free Endpoint package, then runs doctor, configure
dry-run, build, test, install, and package-test for PDU RPC.

The RPC layer itself remains transport-agnostic. The CI uses the Core-free Endpoint
package because the PDU RPC tests exercise TCP and do not require Hakoniwa Core.
