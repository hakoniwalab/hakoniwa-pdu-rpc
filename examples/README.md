# Examples

This directory contains small runnable RPC and Action pairs:

- `rpc_server.cpp`: RPC server for `Service/Add`
- `rpc_client.cpp`: RPC client for `Service/Add`
- `action_fibonacci_server.cpp`: Action server for `sample_action_msgs/Fibonacci`
- `action_fibonacci_client.cpp`: sends one Fibonacci Goal and prints Feedback and Result

## Build

From repository root:

```bash
cmake -S . -B build \
  -DHAKO_PDU_ENDPOINT_PREFIX=/usr/local/hakoniwa \
  -DHAKO_PDU_RPC_BUILD_EXAMPLES=ON
cmake --build build
```

## Run the RPC example

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

## Run the Fibonacci Action example

The Action example uses the user-facing manifest in
`config/sample/action.json`. Generate the Endpoint runtime files before
starting either process:

```bash
python tools/generate_action_config.py \
  --config config/sample/action.json \
  --output .hako/action
```

Open two terminals at the repository root.

Terminal A starts the server:

```bash
build/examples/hakoniwa_pdu_action_fibonacci_server
```

Terminal B sends one Goal. The argument is the requested sequence length and
must be in the range `1..47` because the sample message uses signed 32-bit
integers.

```bash
build/examples/hakoniwa_pdu_action_fibonacci_client 10
```

The Client shows the complete Action lifecycle:

```text
Sent Fibonacci Goal: order=10
Goal Response: ACCEPTED
Feedback: [0, 1, 1]
...
Result: [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
```

The Client waits up to five seconds for the asynchronous TCP connection before
sending the Goal. If it reports a connection timeout, confirm that Terminal A
is still running and that both processes use the same generated configuration.

The Client exits after receiving the Result. Stop the Server with `Ctrl+C`.

The default generated paths are:

- Action runtime config: `.hako/action/resolved-action.json`
- EndpointContainer config: `.hako/action/endpoints.json`
- TCP port: `54011`

Both executables also accept explicit generated paths:

```bash
build/examples/hakoniwa_pdu_action_fibonacci_server \
  /path/to/resolved-action.json \
  /path/to/endpoints.json

build/examples/hakoniwa_pdu_action_fibonacci_client \
  10 \
  /path/to/resolved-action.json \
  /path/to/endpoints.json
```

This first example intentionally demonstrates one Goal through Goal Response,
Feedback, and Result. The Cancel lifecycle remains covered by the native TCP
contract test.
