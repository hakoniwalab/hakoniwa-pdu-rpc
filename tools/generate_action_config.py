#!/usr/bin/env python3
"""Generate resolved Action and Endpoint TCP configuration files."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any


class ConfigurationError(ValueError):
    pass


DEFAULT_ACTION_BUFFER_HEAP_SIZE = 1024 * 1024
MAX_ACTION_BUFFER_HEAP_SIZE = 2**31 - 1


def _object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigurationError(f"{path} must be an object")
    return value


def _text(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value:
        raise ConfigurationError(f"{path} must be a non-empty string")
    return value


def _port(value: Any, path: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or not 1 <= value <= 65535:
        raise ConfigurationError(f"{path} must be an integer in 1..65535")
    return value


def _buffer_heap(action: dict[str, Any], index: int) -> dict[str, int]:
    path = f"actions[{index}].bufferHeap"
    explicitly_configured = "bufferHeap" in action
    configured = action.get("bufferHeap")
    if not explicitly_configured:
        print(
            f"WARNING: {path} is not configured; using defaults "
            f"requestSize={DEFAULT_ACTION_BUFFER_HEAP_SIZE}, "
            f"responseSize={DEFAULT_ACTION_BUFFER_HEAP_SIZE}, "
            f"feedbackSize={DEFAULT_ACTION_BUFFER_HEAP_SIZE}. "
            "Explicit values are recommended for production use.",
            file=sys.stderr,
        )
        configured = {}
    else:
        configured = _object(configured, path)

    allowed = {"requestSize", "responseSize", "feedbackSize"}
    unknown = sorted(set(configured) - allowed)
    if unknown:
        raise ConfigurationError(f"{path} has unknown fields: {', '.join(unknown)}")

    resolved: dict[str, int] = {}
    missing: list[str] = []
    for key in ("requestSize", "responseSize", "feedbackSize"):
        value = configured.get(key, DEFAULT_ACTION_BUFFER_HEAP_SIZE)
        if key not in configured:
            missing.append(key)
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
            or value > MAX_ACTION_BUFFER_HEAP_SIZE
        ):
            raise ConfigurationError(
                f"{path}.{key} must be an integer in "
                f"0..{MAX_ACTION_BUFFER_HEAP_SIZE}"
            )
        resolved[key] = value

    if explicitly_configured and missing:
        defaults = ", ".join(
            f"{key}={DEFAULT_ACTION_BUFFER_HEAP_SIZE}" for key in missing
        )
        print(
            f"WARNING: {path} omits {', '.join(missing)}; using defaults: "
            f"{defaults}. Explicit values are recommended for production use.",
            file=sys.stderr,
        )
    return resolved


def _safe_id(value: str) -> str:
    normalized = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-")
    if not normalized:
        raise ConfigurationError(f"nodeId cannot produce an Endpoint ID: {value!r}")
    return normalized


def _endpoint_id(node_id: str) -> str:
    return f"{_safe_id(node_id)}-action-tcp"


def _channels(action_type: str, slot_count: int) -> list[dict[str, Any]]:
    channels: list[dict[str, Any]] = []
    suffixes = ("Request", "Response", "Feedback")
    for slot in range(slot_count):
        for offset, suffix in enumerate(suffixes):
            channels.append(
                {
                    "slotIndex": slot,
                    "kind": suffix.lower(),
                    "channelId": slot * 3 + offset,
                    "channelName": f"Slot{slot}{suffix}",
                    "packetType": f"{action_type}Action{suffix}",
                }
            )
    return channels


def _validate_action(entry: Any, index: int) -> dict[str, Any]:
    action = _object(entry, f"actions[{index}]")
    allowed = {
        "name",
        "type",
        "slotCount",
        "bufferHeap",
        "clientEndpoint",
        "serverEndpoint",
    }
    unknown = sorted(set(action) - allowed)
    if unknown:
        raise ConfigurationError(f"actions[{index}] has unknown fields: {', '.join(unknown)}")

    name = _text(action.get("name"), f"actions[{index}].name")
    action_type = _text(action.get("type"), f"actions[{index}].type")
    parts = action_type.split("/")
    if len(parts) != 2 or not all(parts):
        raise ConfigurationError(f"actions[{index}].type must use package/ActionName format")

    slot_count = action.get("slotCount")
    if not isinstance(slot_count, int) or isinstance(slot_count, bool) or slot_count <= 0:
        raise ConfigurationError(f"actions[{index}].slotCount must be a positive integer")
    if slot_count > (2**32 - 1) // 3:
        raise ConfigurationError(f"actions[{index}].slotCount exceeds the channel ID range")

    endpoint_nodes: dict[str, str] = {}
    for side in ("clientEndpoint", "serverEndpoint"):
        endpoint = _object(action.get(side), f"actions[{index}].{side}")
        unknown_endpoint = sorted(set(endpoint) - {"nodeId"})
        if unknown_endpoint:
            raise ConfigurationError(
                f"actions[{index}].{side} has unknown fields: {', '.join(unknown_endpoint)}"
            )
        endpoint_nodes[side] = _text(
            endpoint.get("nodeId"), f"actions[{index}].{side}.nodeId"
        )

    return {
        "name": name,
        "type": action_type,
        "slotCount": slot_count,
        "bufferHeap": _buffer_heap(action, index),
        "clientEndpoint": {
            "nodeId": endpoint_nodes["clientEndpoint"],
            "endpointId": _endpoint_id(endpoint_nodes["clientEndpoint"]),
        },
        "serverEndpoint": {
            "nodeId": endpoint_nodes["serverEndpoint"],
            "endpointId": _endpoint_id(endpoint_nodes["serverEndpoint"]),
        },
        "channels": _channels(action_type, slot_count),
    }


def _transport_endpoint(node_id: str, entry: Any) -> dict[str, Any]:
    path = f"transport.endpoints.{node_id}"
    endpoint = _object(entry, path)
    allowed = {"role", "local", "remote", "options"}
    unknown = sorted(set(endpoint) - allowed)
    if unknown:
        raise ConfigurationError(f"{path} has unknown fields: {', '.join(unknown)}")

    role = _text(endpoint.get("role"), f"{path}.role")
    if role not in {"server", "client"}:
        raise ConfigurationError(f"{path}.role must be 'server' or 'client'")

    address_key = "local" if role == "server" else "remote"
    forbidden_key = "remote" if role == "server" else "local"
    if forbidden_key in endpoint:
        raise ConfigurationError(f"{path}.{forbidden_key} is not valid for role '{role}'")
    address = _object(endpoint.get(address_key), f"{path}.{address_key}")
    unknown_address = sorted(set(address) - {"address", "port"})
    if unknown_address:
        raise ConfigurationError(
            f"{path}.{address_key} has unknown fields: {', '.join(unknown_address)}"
        )

    options = endpoint.get("options", {})
    options = _object(options, f"{path}.options")
    return {
        "role": role,
        address_key: {
            "address": _text(address.get("address"), f"{path}.{address_key}.address"),
            "port": _port(address.get("port"), f"{path}.{address_key}.port"),
        },
        "options": options,
    }


def resolve(manifest: dict[str, Any]) -> dict[str, Any]:
    allowed_root = {"version", "actions", "transport"}
    unknown_root = sorted(set(manifest) - allowed_root)
    if unknown_root:
        raise ConfigurationError(f"manifest has unknown fields: {', '.join(unknown_root)}")
    if manifest.get("version") != 1:
        raise ConfigurationError("version must be 1")

    raw_actions = manifest.get("actions")
    if not isinstance(raw_actions, list) or not raw_actions:
        raise ConfigurationError("actions must be a non-empty array")
    actions = [_validate_action(entry, index) for index, entry in enumerate(raw_actions)]
    names = [action["name"] for action in actions]
    if len(names) != len(set(names)):
        raise ConfigurationError("Action names must be unique")

    transport = _object(manifest.get("transport"), "transport")
    allowed_transport = {"protocol", "packetVersion", "queueDepth", "endpoints"}
    unknown_transport = sorted(set(transport) - allowed_transport)
    if unknown_transport:
        raise ConfigurationError(
            f"transport has unknown fields: {', '.join(unknown_transport)}"
        )
    if transport.get("protocol") != "tcp":
        raise ConfigurationError("transport.protocol must be 'tcp' in the initial implementation")
    packet_version = transport.get("packetVersion", "v2")
    if packet_version not in {"v1", "v2"}:
        raise ConfigurationError("transport.packetVersion must be 'v1' or 'v2'")
    queue_depth = transport.get("queueDepth", 64)
    if not isinstance(queue_depth, int) or isinstance(queue_depth, bool) or queue_depth <= 0:
        raise ConfigurationError("transport.queueDepth must be a positive integer")

    raw_endpoints = _object(transport.get("endpoints"), "transport.endpoints")
    referenced_nodes = {
        action[side]["nodeId"]
        for action in actions
        for side in ("clientEndpoint", "serverEndpoint")
    }
    missing = sorted(referenced_nodes - set(raw_endpoints))
    extra = sorted(set(raw_endpoints) - referenced_nodes)
    if missing:
        raise ConfigurationError(
            "transport.endpoints is missing referenced nodeIds: " + ", ".join(missing)
        )
    if extra:
        raise ConfigurationError(
            "transport.endpoints has unreferenced nodeIds: " + ", ".join(extra)
        )

    endpoints = {
        node_id: _transport_endpoint(node_id, raw_endpoints[node_id])
        for node_id in sorted(referenced_nodes)
    }
    roles = {endpoint["role"] for endpoint in endpoints.values()}
    if roles != {"server", "client"}:
        raise ConfigurationError(
            "initial point-to-point TCP configuration requires both server and client roles"
        )
    for action in actions:
        client_node = action["clientEndpoint"]["nodeId"]
        server_node = action["serverEndpoint"]["nodeId"]
        if endpoints[client_node]["role"] == endpoints[server_node]["role"]:
            raise ConfigurationError(
                f"Action '{action['name']}' requires complementary TCP roles for "
                f"{client_node} and {server_node}"
            )

    return {
        "version": 1,
        "actions": actions,
        "transport": {
            "protocol": "tcp",
            "packetVersion": packet_version,
            "queueDepth": queue_depth,
            "endpoints": endpoints,
        },
    }


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    content = json.dumps(value, indent=2, ensure_ascii=False) + "\n"
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def generate(manifest_path: Path, output_dir: Path) -> list[Path]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ConfigurationError(f"failed to read manifest: {error}") from error
    except json.JSONDecodeError as error:
        raise ConfigurationError(f"invalid manifest JSON: {error}") from error
    if not isinstance(manifest, dict):
        raise ConfigurationError("manifest root must be an object")

    resolved = resolve(manifest)
    generated: list[Path] = []

    resolved_path = output_dir / "resolved-action.json"
    _write_json(resolved_path, resolved)
    generated.append(resolved_path)

    queue_path = output_dir / "queue.json"
    _write_json(
        queue_path,
        {
            "type": "buffer",
            "name": "action_queue_buffer",
            "store": {"mode": "queue", "depth": resolved["transport"]["queueDepth"]},
        },
    )
    generated.append(queue_path)

    container_entries: list[dict[str, Any]] = []
    for node_id, deployment in resolved["transport"]["endpoints"].items():
        endpoint_id = _endpoint_id(node_id)
        endpoint_relative = Path("endpoints") / f"{_safe_id(node_id)}.json"
        transport_relative = Path("transport") / f"{_safe_id(node_id)}.json"

        endpoint_path = output_dir / endpoint_relative
        _write_json(
            endpoint_path,
            {
                "name": endpoint_id,
                "cache": "../queue.json",
                "comm": f"../{transport_relative.as_posix()}",
                "recv_cache_write": False,
            },
        )
        generated.append(endpoint_path)

        transport_config: dict[str, Any] = {
            "protocol": "tcp",
            "name": endpoint_id,
            "direction": "inout",
            "role": deployment["role"],
            "comm_raw_version": resolved["transport"]["packetVersion"],
            "options": deployment["options"],
        }
        address_key = "local" if deployment["role"] == "server" else "remote"
        transport_config[address_key] = deployment[address_key]
        transport_path = output_dir / transport_relative
        _write_json(transport_path, transport_config)
        generated.append(transport_path)

        container_entries.append(
            {
                "nodeId": node_id,
                "endpoints": [
                    {
                        "id": endpoint_id,
                        "config_path": endpoint_relative.as_posix(),
                        "direction": "inout",
                        "mode": "action-tcp",
                    }
                ],
            }
        )

    endpoints_path = output_dir / "endpoints.json"
    _write_json(endpoints_path, container_entries)
    generated.append(endpoints_path)
    return generated


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate resolved Hakoniwa Action TCP configuration"
    )
    parser.add_argument("--config", required=True, type=Path, help="User Action manifest")
    parser.add_argument("--output", required=True, type=Path, help="Generated output directory")
    args = parser.parse_args()

    try:
        generated = generate(args.config.resolve(), args.output.resolve())
    except ConfigurationError as error:
        parser.error(str(error))
    for path in generated:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
