#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:  # pragma: no cover - runtime dependency check
    print("Missing dependency: jsonschema. Install with: pip install jsonschema", file=sys.stderr)
    sys.exit(2)


REPO_ROOT = Path(__file__).resolve().parents[1]
SERVICE_SCHEMA = REPO_ROOT / "config" / "schema" / "service-schema.json"
ENDPOINTS_SCHEMA = REPO_ROOT / "config" / "schema" / "endpoints-schema.json"
ENDPOINT_SCHEMA = REPO_ROOT / "hakoniwa-pdu-endpoint" / "config" / "schema" / "endpoint_schema.json"
ENDPOINT_VALIDATOR = REPO_ROOT / "hakoniwa-pdu-endpoint" / "tools" / "validate_json.py"


def load_json(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def resolve_ref(base_dir: Path, raw_value: str) -> Path:
    p = Path(raw_value)
    if p.is_absolute():
        return p
    return (base_dir / p).resolve()


def validate_schema(schema_path: Path, json_path: Path) -> list[str]:
    try:
        schema = load_json(schema_path)
    except json.JSONDecodeError as e:
        return [f"{schema_path}: schema JSON parse error: {e}"]
    except OSError as e:
        return [f"{schema_path}: schema read error: {e}"]

    try:
        data = load_json(json_path)
    except json.JSONDecodeError as e:
        return [f"{json_path}: JSON parse error: {e}"]
    except OSError as e:
        return [f"{json_path}: read error: {e}"]

    validator = jsonschema.Draft7Validator(schema)
    errors = sorted(validator.iter_errors(data), key=lambda e: e.path)
    messages = []
    for e in errors:
        loc = "/".join([str(x) for x in e.path]) or "(root)"
        messages.append(f"{json_path}: {loc}: {e.message}")
    return messages


def collect_endpoint_configs_from_endpoints(endpoints_json, base_dir: Path) -> list[Path]:
    configs = []
    if not isinstance(endpoints_json, list):
        return configs
    for node in endpoints_json:
        if not isinstance(node, dict):
            continue
        for endpoint in node.get("endpoints", []):
            if not isinstance(endpoint, dict):
                continue
            config_path = endpoint.get("config_path")
            if isinstance(config_path, str):
                configs.append(resolve_ref(base_dir, config_path))
    return configs


def check_service_config_paths(service_path: Path, service_json) -> list[str]:
    messages = []
    base_dir = service_path.parent

    endpoints_json = None
    endpoint_config_paths = []

    endpoints_config_path = service_json.get("endpoints_config_path")
    if isinstance(endpoints_config_path, str):
        resolved = resolve_ref(base_dir, endpoints_config_path)
        if not resolved.exists():
            messages.append(f"{service_path}: missing referenced file: {endpoints_config_path}")
        else:
            try:
                endpoints_json = load_json(resolved)
                endpoint_config_paths.extend(
                    collect_endpoint_configs_from_endpoints(endpoints_json, resolved.parent)
                )
            except json.JSONDecodeError as e:
                messages.append(f"{resolved}: JSON parse error: {e}")
            except OSError as e:
                messages.append(f"{resolved}: read error: {e}")

    endpoints_inline = service_json.get("endpoints")
    if isinstance(endpoints_inline, list):
        endpoints_json = endpoints_inline
        endpoint_config_paths.extend(
            collect_endpoint_configs_from_endpoints(endpoints_inline, base_dir)
        )

    pdu_config_path = service_json.get("pdu_config_path")
    if isinstance(pdu_config_path, str):
        resolved = resolve_ref(base_dir, pdu_config_path)
        if not resolved.exists():
            messages.append(f"{service_path}: missing referenced file: {pdu_config_path}")

    for config_path in endpoint_config_paths:
        if not config_path.exists():
            messages.append(f"{service_path}: missing referenced file: {config_path}")

    return messages


def validate_endpoints_config(endpoints_path: Path) -> list[str]:
    messages = validate_schema(ENDPOINTS_SCHEMA, endpoints_path)
    if messages:
        return messages

    try:
        endpoints_json = load_json(endpoints_path)
    except json.JSONDecodeError as e:
        return [f"{endpoints_path}: JSON parse error: {e}"]
    except OSError as e:
        return [f"{endpoints_path}: read error: {e}"]

    endpoint_configs = collect_endpoint_configs_from_endpoints(endpoints_json, endpoints_path.parent)
    for config_path in endpoint_configs:
        if not config_path.exists():
            messages.append(f"{endpoints_path}: missing referenced file: {config_path}")
    return messages


def validate_endpoint_config(endpoint_path: Path) -> list[str]:
    if not ENDPOINT_SCHEMA.exists():
        return [f"{ENDPOINT_SCHEMA}: endpoint schema not found"]
    if not ENDPOINT_VALIDATOR.exists():
        return [f"{ENDPOINT_VALIDATOR}: validator script not found"]

    cmd = [
        sys.executable,
        str(ENDPOINT_VALIDATOR),
        "--schema",
        str(ENDPOINT_SCHEMA),
        "--check-paths",
        str(endpoint_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    messages = []
    if proc.stdout:
        for line in proc.stdout.strip().splitlines():
            messages.append(line)
    if proc.stderr:
        for line in proc.stderr.strip().splitlines():
            messages.append(line)
    if proc.returncode not in (0, 1):
        messages.append(f"{endpoint_path}: validator failed with exit code {proc.returncode}")
    return messages


def iter_service_files(paths):
    for p in paths:
        path = Path(p)
        if path.is_dir():
            for child in sorted(path.rglob("*.json")):
                yield child
        else:
            yield path


def main():
    parser = argparse.ArgumentParser(
        description="Validate RPC service configs and referenced endpoint configs."
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="Service config JSON file(s) or directories to scan.",
    )
    parser.add_argument(
        "--skip-endpoint-validation",
        action="store_true",
        help="Skip validating endpoint configs via hakoniwa-pdu-endpoint.",
    )
    args = parser.parse_args()

    had_error = False
    for service_path in iter_service_files(args.paths):
        messages = []
        messages.extend(validate_schema(SERVICE_SCHEMA, service_path))

        try:
            service_json = load_json(service_path)
        except json.JSONDecodeError as e:
            messages.append(f"{service_path}: JSON parse error: {e}")
            service_json = None
        except OSError as e:
            messages.append(f"{service_path}: read error: {e}")
            service_json = None

        if service_json is not None:
            messages.extend(check_service_config_paths(service_path, service_json))

            endpoints_config_path = service_json.get("endpoints_config_path")
            if isinstance(endpoints_config_path, str):
                endpoints_path = resolve_ref(service_path.parent, endpoints_config_path)
                messages.extend(validate_endpoints_config(endpoints_path))

                if not args.skip_endpoint_validation:
                    try:
                        endpoints_json = load_json(endpoints_path)
                    except Exception:
                        endpoints_json = None
                    if endpoints_json:
                        endpoint_configs = collect_endpoint_configs_from_endpoints(
                            endpoints_json, endpoints_path.parent
                        )
                        for endpoint_path in endpoint_configs:
                            messages.extend(validate_endpoint_config(endpoint_path))

            endpoints_inline = service_json.get("endpoints")
            if isinstance(endpoints_inline, list) and not args.skip_endpoint_validation:
                endpoint_configs = collect_endpoint_configs_from_endpoints(
                    endpoints_inline, service_path.parent
                )
                for endpoint_path in endpoint_configs:
                    messages.extend(validate_endpoint_config(endpoint_path))

        if messages:
            had_error = True
            for msg in messages:
                print(msg, file=sys.stderr)
        else:
            print(f"{service_path}: OK")

    return 1 if had_error else 0


if __name__ == "__main__":
    raise SystemExit(main())
