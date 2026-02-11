# Minimal RPC Config Set

This folder is a compact config set for the `AddTwoInts` sample.

## Files

- `simple-service.json`:
  - 1 service (`Service/Add`)
  - 1 server endpoint
  - 1 client (`TestClient`)
  - fixed request/response channels (`1`, `2`)
- `endpoints.json`:
  - maps `server_node`/`client_node` to endpoint config files
- `server_endpoint.json`, `client_endpoint.json`:
  - endpoint runtime settings
- `queue.json`:
  - in-process queue buffer definition
- `tcp_server_inout_comm.json`, `tcp_client_inout_comm.json`:
  - transport settings (TCP loopback, port `54001`)

## Notes

- `pdu_def_path` points to `../pdudef.json` to reuse the existing registry definition.
- Keep `nodeId`/`endpointId` in `simple-service.json` and `endpoints.json` aligned.
- Keep channel IDs unique within one service.

## Validate

Run from repository root:

```bash
PYTHONPATH=python:$PYTHONPATH \
python -m hakoniwa_pdu_rpc.validate_configs config/sample/minimal/simple-service.json --skip-endpoint-validation
```

