# Tools

## Config Validation

Validate service configs, endpoints configs, and endpoint definitions.

```bash
python tools/validate_configs.py config/sample/simple-service.json
python tools/validate_configs.py test/configs/service_config.json
```

Options:
- `--skip-endpoint-validation`: Skip validating endpoint configs via `hakoniwa-pdu-endpoint`.

Notes:
- Requires `jsonschema` (`pip install jsonschema`).
