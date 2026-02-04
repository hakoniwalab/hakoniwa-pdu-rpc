# Tools

## Config Validation

Validate service configs, endpoints configs, and endpoint definitions.

```bash
python tools/validate_configs.py config/sample/simple-service.json
python tools/validate_configs.py test/configs/service_config.json
```

Options:
- `--skip-endpoint-validation`: Skip validating endpoint configs via installed `hakoniwa-pdu-endpoint` validator.

Notes:
- Requires `jsonschema` (`pip install jsonschema`).
- The endpoint validator is expected to be installed with `hakoniwa-pdu-endpoint`.
- Set `PYTHONPATH` to include `/usr/local/hakoniwa/share/hakoniwa-pdu-endpoint/python`.
- If the endpoint schema is not found, set `HAKO_PDU_ENDPOINT_SCHEMA` to the installed schema path.
