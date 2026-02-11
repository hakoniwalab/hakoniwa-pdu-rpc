# Examples

This directory contains the smallest runnable RPC pair:

- `rpc_server.cpp`: RPC server for `Service/Add`
- `rpc_client.cpp`: RPC client for `Service/Add`

## Build

From repository root:

```bash
cmake -S . -B build \
  -DHAKO_PDU_ENDPOINT_PREFIX=/usr/local/hakoniwa \
  -DHAKO_PDU_RPC_BUILD_EXAMPLES=ON
cmake --build build
```

## Run

Open 2 terminals from repository root.

Terminal A:

```bash
build/examples/hakoniwa_pdu_rpc_server
```

Terminal B:

```bash
build/examples/hakoniwa_pdu_rpc_client 1000000
```

Then type two integers (e.g. `5 7`) and confirm:

```text
sum=12
```

## Configs Used By Default

- Service config: `config/sample/simple-service.json`
- Endpoints config: `config/sample/endpoints.json`

## Common Failures

- Build cannot find `hakoniwa-pdu-endpoint`:
  - Set `-DHAKO_PDU_ENDPOINT_PREFIX=/path/to/hakoniwa`.
- Runtime shared library not found:
  - Set `LD_LIBRARY_PATH` (Linux) or `DYLD_LIBRARY_PATH` (macOS) to include Hakoniwa libs.
- Client times out:
  - Start server first.
  - Confirm both processes use the same config files and TCP port (`54001`).

