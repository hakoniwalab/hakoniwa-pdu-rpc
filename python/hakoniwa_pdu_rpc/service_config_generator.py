"""Generate native RPC Service and Endpoint TCP configuration files."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any


class ConfigurationError(ValueError):
    pass


PDU_METADATA_SIZE = 24


def _object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigurationError(f"{path} must be an object")
    return value


def _text(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value:
        raise ConfigurationError(f"{path} must be a non-empty string")
    return value


def _positive_int(value: Any, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ConfigurationError(f"{path} must be a positive integer")
    return value


def _nonnegative_int(value: Any, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ConfigurationError(f"{path} must be a non-negative integer")
    return value


def _port(value: Any, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65535:
        raise ConfigurationError(f"{path} must be an integer in 1..65535")
    return value


def _safe_id(value: str) -> str:
    normalized = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-")
    if not normalized:
        raise ConfigurationError(f"value cannot produce a safe ID: {value!r}")
    return normalized


def _service_key(service_name: str) -> str:
    tail = service_name.rsplit("/", 1)[-1]
    snake = re.sub(r"(?<!^)(?=[A-Z])", "_", tail).lower()
    normalized = re.sub(r"[^a-z0-9_]+", "_", snake).strip("_")
    if not normalized:
        raise ConfigurationError(
            f"Service name cannot produce a client name: {service_name!r}"
        )
    return normalized


def _endpoint_id(node_id: str) -> str:
    return f"{_safe_id(node_id)}-service-tcp"


def _endpoint_ref(entry: dict[str, Any], index: int, field: str) -> dict[str, str]:
    path = f"services[{index}].{field}"
    endpoint = _object(entry.get(field), path)
    unknown = sorted(set(endpoint) - {"nodeId"})
    if unknown:
        raise ConfigurationError(f"{path} has unknown fields: {', '.join(unknown)}")
    node_id = _text(endpoint.get("nodeId"), f"{path}.nodeId")
    return {"nodeId": node_id, "endpointId": _endpoint_id(node_id)}


def _validate_service(entry: Any, index: int) -> dict[str, Any]:
    service = _object(entry, f"services[{index}]")
    allowed = {
        "name",
        "type",
        "maxClients",
        "clientNamePrefix",
        "packetSize",
        "bufferHeap",
        "clientEndpoint",
        "serverEndpoint",
    }
    unknown = sorted(set(service) - allowed)
    if unknown:
        raise ConfigurationError(
            f"services[{index}] has unknown fields: {', '.join(unknown)}"
        )

    name = _text(service.get("name"), f"services[{index}].name")
    service_type = _text(service.get("type"), f"services[{index}].type")
    if len(service_type.split("/")) != 2 or not all(service_type.split("/")):
        raise ConfigurationError(
            f"services[{index}].type must use package/ServiceName format"
        )
    max_clients = _positive_int(
        service.get("maxClients"), f"services[{index}].maxClients"
    )
    prefix = service.get("clientNamePrefix", f"hako_rpc_{_service_key(name)}")
    prefix = _text(prefix, f"services[{index}].clientNamePrefix")

    packet_size = _object(
        service.get("packetSize"), f"services[{index}].packetSize"
    )
    unknown_packet = sorted(
        set(packet_size) - {"requestBaseSize", "responseBaseSize"}
    )
    if unknown_packet:
        raise ConfigurationError(
            f"services[{index}].packetSize has unknown fields: "
            f"{', '.join(unknown_packet)}"
        )
    request_base = _positive_int(
        packet_size.get("requestBaseSize"),
        f"services[{index}].packetSize.requestBaseSize",
    )
    response_base = _positive_int(
        packet_size.get("responseBaseSize"),
        f"services[{index}].packetSize.responseBaseSize",
    )

    heap = service.get("bufferHeap", {})
    heap = _object(heap, f"services[{index}].bufferHeap")
    unknown_heap = sorted(set(heap) - {"requestSize", "responseSize"})
    if unknown_heap:
        raise ConfigurationError(
            f"services[{index}].bufferHeap has unknown fields: "
            f"{', '.join(unknown_heap)}"
        )
    request_heap = _nonnegative_int(
        heap.get("requestSize", 0),
        f"services[{index}].bufferHeap.requestSize",
    )
    response_heap = _nonnegative_int(
        heap.get("responseSize", 0),
        f"services[{index}].bufferHeap.responseSize",
    )

    client_endpoint = _endpoint_ref(service, index, "clientEndpoint")
    server_endpoint = _endpoint_ref(service, index, "serverEndpoint")
    if client_endpoint["nodeId"] == server_endpoint["nodeId"]:
        raise ConfigurationError(
            f"services[{index}] clientEndpoint and serverEndpoint must differ"
        )

    clients = [
        {
            "name": f"{prefix}_{client_index}",
            "requestChannelId": client_index * 2,
            "responseChannelId": client_index * 2 + 1,
            "client_endpoint": client_endpoint,
        }
        for client_index in range(max_clients)
    ]
    return {
        "name": name,
        "type": service_type,
        "maxClients": max_clients,
        "pduSize": {
            "server": {
                "heapSize": response_heap,
                "baseSize": request_base,
            },
            "client": {
                "heapSize": request_heap,
                "baseSize": response_base,
            },
        },
        "serverEndpoint": server_endpoint,
        "clients": clients,
    }


def _transport_endpoint(node_id: str, entry: Any) -> dict[str, Any]:
    path = f"transport.endpoints.{node_id}"
    endpoint = _object(entry, path)
    unknown = sorted(set(endpoint) - {"role", "local", "remote", "options"})
    if unknown:
        raise ConfigurationError(f"{path} has unknown fields: {', '.join(unknown)}")
    role = _text(endpoint.get("role"), f"{path}.role")
    if role not in {"server", "client"}:
        raise ConfigurationError(f"{path}.role must be 'server' or 'client'")
    address_key = "local" if role == "server" else "remote"
    forbidden_key = "remote" if role == "server" else "local"
    if forbidden_key in endpoint:
        raise ConfigurationError(
            f"{path}.{forbidden_key} is not valid for role '{role}'"
        )
    address = _object(endpoint.get(address_key), f"{path}.{address_key}")
    unknown_address = sorted(set(address) - {"address", "port"})
    if unknown_address:
        raise ConfigurationError(
            f"{path}.{address_key} has unknown fields: {', '.join(unknown_address)}"
        )
    options = _object(endpoint.get("options", {}), f"{path}.options")
    return {
        "role": role,
        address_key: {
            "address": _text(
                address.get("address"), f"{path}.{address_key}.address"
            ),
            "port": _port(address.get("port"), f"{path}.{address_key}.port"),
        },
        "options": options,
    }


def resolve(manifest: dict[str, Any]) -> dict[str, Any]:
    unknown_root = sorted(set(manifest) - {"version", "services", "transport"})
    if unknown_root:
        raise ConfigurationError(
            f"manifest has unknown fields: {', '.join(unknown_root)}"
        )
    if manifest.get("version") != 1:
        raise ConfigurationError("version must be 1")

    raw_services = manifest.get("services")
    if not isinstance(raw_services, list) or not raw_services:
        raise ConfigurationError("services must be a non-empty array")
    services = [
        _validate_service(entry, index)
        for index, entry in enumerate(raw_services)
    ]
    names = [service["name"] for service in services]
    if len(names) != len(set(names)):
        raise ConfigurationError("Service names must be unique")

    transport = _object(manifest.get("transport"), "transport")
    unknown_transport = sorted(
        set(transport) - {"protocol", "packetVersion", "queueDepth", "endpoints"}
    )
    if unknown_transport:
        raise ConfigurationError(
            f"transport has unknown fields: {', '.join(unknown_transport)}"
        )
    if transport.get("protocol") != "tcp":
        raise ConfigurationError(
            "transport.protocol must be 'tcp' in the initial implementation"
        )
    packet_version = transport.get("packetVersion", "v2")
    if packet_version not in {"v1", "v2"}:
        raise ConfigurationError("transport.packetVersion must be 'v1' or 'v2'")
    queue_depth = _positive_int(
        transport.get("queueDepth", 64), "transport.queueDepth"
    )

    raw_endpoints = _object(transport.get("endpoints"), "transport.endpoints")
    referenced_nodes = {
        endpoint["nodeId"]
        for service in services
        for endpoint in (
            service["serverEndpoint"],
            service["clients"][0]["client_endpoint"],
        )
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
    for service in services:
        client_node = service["clients"][0]["client_endpoint"]["nodeId"]
        server_node = service["serverEndpoint"]["nodeId"]
        if endpoints[client_node]["role"] != "client":
            raise ConfigurationError(
                f"Service '{service['name']}' clientEndpoint must have transport role 'client'"
            )
        if endpoints[server_node]["role"] != "server":
            raise ConfigurationError(
                f"Service '{service['name']}' serverEndpoint must have transport role 'server'"
            )

    return {
        "version": 1,
        "services": services,
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

    resolved_path = output_dir / "resolved-service.json"
    _write_json(resolved_path, resolved)
    generated.append(resolved_path)

    server_services = {
        "pduMetaDataSize": PDU_METADATA_SIZE,
        "services": [
            {
                **{key: value for key, value in service.items() if key != "serverEndpoint"},
                "server_endpoints": [service["serverEndpoint"]],
            }
            for service in resolved["services"]
        ],
    }
    client_services = {
        "pduMetaDataSize": PDU_METADATA_SIZE,
        "services": [
            {
                **{key: value for key, value in service.items() if key != "serverEndpoint"},
                "server_endpoints": [],
            }
            for service in resolved["services"]
        ],
    }
    server_path = output_dir / "rpc-server-services.json"
    client_path = output_dir / "rpc-client-services.json"
    _write_json(server_path, server_services)
    _write_json(client_path, client_services)
    generated.extend((server_path, client_path))

    queue_path = output_dir / "queue.json"
    _write_json(
        queue_path,
        {
            "type": "buffer",
            "name": "service_queue_buffer",
            "store": {
                "mode": "queue",
                "depth": resolved["transport"]["queueDepth"],
            },
        },
    )
    generated.append(queue_path)
    pdu_def_path = output_dir / "pdudef.json"
    _write_json(
        pdu_def_path,
        {
            "robots": [
                {
                    "name": "HakoniwaRpcService",
                    "rpc_pdu_readers": [],
                    "rpc_pdu_writers": [],
                    "shm_pdu_readers": [],
                    "shm_pdu_writers": [],
                }
            ]
        },
    )
    generated.append(pdu_def_path)

    expected_by_server: dict[str, int] = {}
    for service in resolved["services"]:
        node_id = service["serverEndpoint"]["nodeId"]
        expected_by_server[node_id] = (
            expected_by_server.get(node_id, 0) + service["maxClients"]
        )

    endpoint_entries = []
    for node_id, deployment in resolved["transport"]["endpoints"].items():
        endpoint_id = _endpoint_id(node_id)
        safe_node = _safe_id(node_id)
        endpoint_relative = Path("endpoints") / f"{safe_node}.json"
        transport_relative = Path("transport") / f"{safe_node}.json"
        _write_json(
            output_dir / endpoint_relative,
            {
                "name": endpoint_id,
                "pdu_def_path": "../pdudef.json",
                "cache": "../queue.json",
                "comm": f"../{transport_relative.as_posix()}",
            },
        )
        generated.append(output_dir / endpoint_relative)

        transport_config: dict[str, Any] = {
            "protocol": "tcp",
            "name": endpoint_id,
            "direction": "inout",
            "comm_raw_version": resolved["transport"]["packetVersion"],
            "options": deployment["options"],
        }
        if deployment["role"] == "server":
            transport_config["local"] = deployment["local"]
            transport_config["expected_clients"] = expected_by_server[node_id]
        else:
            transport_config["role"] = "client"
            transport_config["remote"] = deployment["remote"]
        _write_json(output_dir / transport_relative, transport_config)
        generated.append(output_dir / transport_relative)

        endpoint_entries.append(
            {
                "nodeId": node_id,
                "endpoints": [
                    {
                        "id": endpoint_id,
                        "config_path": endpoint_relative.as_posix(),
                    }
                ],
            }
        )

    endpoints_path = output_dir / "endpoints.json"
    _write_json(endpoints_path, endpoint_entries)
    generated.append(endpoints_path)
    return generated


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate resolved Hakoniwa RPC Service TCP configuration"
    )
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
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
