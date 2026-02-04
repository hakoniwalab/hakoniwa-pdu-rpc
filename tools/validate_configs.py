#!/usr/bin/env python3
import argparse
import importlib.util
import json
import os
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
DEFAULT_ENDPOINT_SCHEMA = Path("/usr/local/hakoniwa/share/hakoniwa-pdu-endpoint/schema/endpoint_schema.json")
ENDPOINT_SCHEMA_ENV = os.environ.get("HAKO_PDU_ENDPOINT_SCHEMA")
ENDPOINT_SCHEMA = Path(ENDPOINT_SCHEMA_ENV) if ENDPOINT_SCHEMA_ENV else DEFAULT_ENDPOINT_SCHEMA
ENDPOINT_VALIDATOR_MODULE = "hakoniwa_pdu_endpoint.validate_json"


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


def build_endpoint_index(endpoints_json) -> dict[str, set[str]]:
    idx: dict[str, set[str]] = {}
    if not isinstance(endpoints_json, list):
        return idx
    for i, node in enumerate(endpoints_json):
        if not isinstance(node, dict):
            continue
        node_id = node.get("nodeId")
        if not isinstance(node_id, str) or not node_id:
            continue
        idx.setdefault(node_id, set())
        node_eps = node.get("endpoints")
        if not isinstance(node_eps, list):
            continue
        for ep in node_eps:
            if not isinstance(ep, dict):
                continue
            ep_id = ep.get("id")
            if isinstance(ep_id, str) and ep_id:
                idx[node_id].add(ep_id)
    return idx


def check_rpc_semantics(service_path: Path, service_json, endpoints_json) -> list[str]:
    messages: list[str] = []

    # pduMetaDataSize fixed value
    metadata_size = service_json.get("pduMetaDataSize")
    if isinstance(metadata_size, int):
        if metadata_size != 24:
            messages.append(f"{service_path}: rpc.pduMetaDataSize: must be 24")

    endpoint_idx = build_endpoint_index(endpoints_json)

    # services list checks
    svcs = service_json.get("services")
    if not isinstance(svcs, list):
        return messages

    service_names: set[str] = set()
    for si, svc in enumerate(svcs):
        if not isinstance(svc, dict):
            continue

        sname = svc.get("name")
        if isinstance(sname, str) and sname:
            if sname in service_names:
                messages.append(f"{service_path}: rpc.services: duplicate service name '{sname}'")
            service_names.add(sname)
        else:
            sname = f"<services[{si}]>"

        max_clients = svc.get("maxClients")
        clients = svc.get("clients")
        if isinstance(max_clients, int) and isinstance(clients, list):
            if len(clients) > max_clients:
                messages.append(
                    f"{service_path}: rpc.services[{si}] '{sname}': clients.length({len(clients)}) > maxClients({max_clients})"
                )

        # server endpoints
        server_endpoints = svc.get("server_endpoints")
        if server_endpoints is None:
            server_endpoint = svc.get("server_endpoint")
            server_endpoints = [server_endpoint] if isinstance(server_endpoint, dict) else None
        if isinstance(server_endpoints, list):
            for ei, se in enumerate(server_endpoints):
                if not isinstance(se, dict):
                    continue
                snode = se.get("nodeId")
                seid = se.get("endpointId")
                if isinstance(snode, str) and isinstance(seid, str) and snode and seid:
                    if snode not in endpoint_idx:
                        messages.append(
                            f"{service_path}: rpc.services[{si}] '{sname}': server_endpoints[{ei}]: "
                            f"server nodeId '{snode}' not found in rpc.endpoints"
                        )
                    elif seid not in endpoint_idx[snode]:
                        messages.append(
                            f"{service_path}: rpc.services[{si}] '{sname}': server_endpoints[{ei}]: "
                            f"server endpointId '{seid}' not found under node '{snode}'"
                        )

        # client endpoint + channel checks
        if isinstance(clients, list):
            used_channels: set[int] = set()
            client_names: set[str] = set()
            for ci, c in enumerate(clients):
                if not isinstance(c, dict):
                    continue
                cname = c.get("name")
                if isinstance(cname, str) and cname:
                    if cname in client_names:
                        messages.append(
                            f"{service_path}: rpc.services[{si}] '{sname}': duplicate client name '{cname}'"
                        )
                    client_names.add(cname)

                for key in ("requestChannelId", "responseChannelId"):
                    channel_id = c.get(key)
                    if isinstance(channel_id, int):
                        if channel_id in used_channels:
                            messages.append(
                                f"{service_path}: rpc.services[{si}] '{sname}': channel collision: {channel_id} ({key})"
                            )
                        used_channels.add(channel_id)

                ce = c.get("client_endpoint")
                if isinstance(ce, dict):
                    cnode = ce.get("nodeId")
                    ceid = ce.get("endpointId")
                    if isinstance(cnode, str) and isinstance(ceid, str) and cnode and ceid:
                        if cnode not in endpoint_idx:
                            messages.append(
                                f"{service_path}: rpc.services[{si}] '{sname}': client nodeId '{cnode}' not found in rpc.endpoints"
                            )
                        elif ceid not in endpoint_idx[cnode]:
                            messages.append(
                                f"{service_path}: rpc.services[{si}] '{sname}': client endpointId '{ceid}' "
                                f"not found under node '{cnode}'"
                            )

    return messages


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


def _resolve_endpoint_schema(endpoint_schema_path: str | None) -> Path:
    if endpoint_schema_path:
        return Path(endpoint_schema_path)
    return ENDPOINT_SCHEMA


def validate_endpoint_config(endpoint_path: Path, endpoint_schema_path: str | None) -> list[str]:
    schema_path = _resolve_endpoint_schema(endpoint_schema_path)
    if not schema_path.exists():
        return [
            f"{schema_path}: endpoint schema not found",
            "Set HAKO_PDU_ENDPOINT_SCHEMA or pass --endpoint-schema.",
        ]
    if importlib.util.find_spec(ENDPOINT_VALIDATOR_MODULE) is None:
        return [
            f"{ENDPOINT_VALIDATOR_MODULE}: module not found",
            "Set PYTHONPATH to include /usr/local/hakoniwa/share/hakoniwa-pdu-endpoint/python.",
        ]

    cmd = [
        sys.executable,
        "-m",
        ENDPOINT_VALIDATOR_MODULE,
        "--schema",
        str(schema_path),
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
        help="Skip validating endpoint configs via installed hakoniwa-pdu-endpoint validator.",
    )
    parser.add_argument(
        "--endpoint-schema",
        help="Path to installed hakoniwa-pdu-endpoint endpoint_schema.json",
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
                            messages.extend(validate_endpoint_config(endpoint_path, args.endpoint_schema))
                        messages.extend(check_rpc_semantics(service_path, service_json, endpoints_json))

            endpoints_inline = service_json.get("endpoints")
            if isinstance(endpoints_inline, list):
                if not args.skip_endpoint_validation:
                    endpoint_configs = collect_endpoint_configs_from_endpoints(
                        endpoints_inline, service_path.parent
                    )
                    for endpoint_path in endpoint_configs:
                        messages.extend(validate_endpoint_config(endpoint_path, args.endpoint_schema))
                messages.extend(check_rpc_semantics(service_path, service_json, endpoints_inline))

        if messages:
            had_error = True
            for msg in messages:
                print(msg, file=sys.stderr)
        else:
            print(f"{service_path}: OK")

    return 1 if had_error else 0


if __name__ == "__main__":
    raise SystemExit(main())
