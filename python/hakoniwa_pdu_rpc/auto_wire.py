from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from importlib import import_module
import json
from pathlib import Path
from typing import Any, Callable, Mapping

from .client import RpcClient
from .cffi_api import RpcServer, ServerEvent
from .future import RpcFuture
from .mux_server import RpcMuxServer


DEFAULT_SERVICE_PACKAGES = (
    "hakoniwa_pdu.pdu_msgs.hako_srv_msgs",
    "pdu.python.hako_srv_msgs",
)


@dataclass(frozen=True)
class ServiceWire:
    request_packet_type: type
    response_packet_type: type
    request_encode: Callable[[Any], bytes]
    request_decode: Callable[[bytes], Any]
    response_encode: Callable[[Any], bytes]
    response_decode: Callable[[bytes], Any]


class RpcServiceStatus(IntEnum):
    NONE = 0
    DOING = 1
    CANCELING = 2
    DONE = 3
    ERROR = 4


class RpcServiceResultCode(IntEnum):
    OK = 0
    ERROR = 1
    CANCELED = 2
    INVALID = 3
    BUSY = 4
    NOT_SUPPORTED = 5


class TypedRpcServiceError(RuntimeError):
    def __init__(
        self,
        service_name: str,
        status: int,
        result_code: int,
    ) -> None:
        self.service_name = service_name
        self.status = int(status)
        self.result_code = int(result_code)
        super().__init__(
            "RPC service returned an error: "
            f"service={service_name!r}, status={self.status}, "
            f"result_code={self.result_code}"
        )


@dataclass(frozen=True)
class TypedRpcServerPollResult:
    event: ServerEvent
    service_name: str
    client_name: str
    request_token: int
    request_body: Any | None = None


class TypedRpcRequestDecodeError(RuntimeError):
    def __init__(
        self,
        request: TypedRpcServerPollResult,
        cause: BaseException,
    ) -> None:
        self.request = request
        self.__cause__ = cause
        super().__init__(
            "failed to decode typed RPC request: "
            f"service={request.service_name!r}, "
            f"client={request.client_name!r}, "
            f"request_token={request.request_token}, error={cause}"
        )


def load_service_wire(
    service_type: str,
    package: str | None = None,
) -> ServiceWire:
    """Load generated PDU Registry packet types and converters by convention.

    When ``package`` is omitted, prefer the installed ``hakoniwa-pdu`` package
    layout used by hakoniwa-pdu-ros, then fall back to the Registry source-tree
    layout used by this repository's submodule tests.
    """
    if "/" in service_type:
        package_name, type_name = _split_service_type(service_type)
        packages = (
            (package,)
            if package is not None
            else (
                f"hakoniwa_pdu.pdu_msgs.{package_name}",
                f"pdu.python.{package_name}",
            )
        )
    else:
        type_name = service_type
        packages = (package,) if package is not None else DEFAULT_SERVICE_PACKAGES
    errors: list[BaseException] = []

    for candidate in packages:
        try:
            return _load_service_wire_from_package(type_name, candidate)
        except (ImportError, AttributeError) as error:
            errors.append(error)

    attempted = ", ".join(repr(candidate) for candidate in packages)
    raise RuntimeError(
        "failed to load generated PDU Registry service components: "
        f"service_type={service_type!r}, packages=[{attempted}]"
    ) from errors[-1]


def _load_service_wire_from_package(
    service_type: str,
    package: str,
) -> ServiceWire:
    request_name = f"{service_type}RequestPacket"
    response_name = f"{service_type}ResponsePacket"

    request_type_module = import_module(f"{package}.pdu_pytype_{request_name}")
    response_type_module = import_module(f"{package}.pdu_pytype_{response_name}")
    request_converter_module = import_module(f"{package}.pdu_conv_{request_name}")
    response_converter_module = import_module(f"{package}.pdu_conv_{response_name}")

    request_encoder = getattr(
        request_converter_module, f"py_to_pdu_{request_name}"
    )
    response_encoder = getattr(
        response_converter_module, f"py_to_pdu_{response_name}"
    )

    return ServiceWire(
        request_packet_type=getattr(request_type_module, request_name),
        response_packet_type=getattr(response_type_module, response_name),
        request_encode=lambda packet: bytes(request_encoder(packet)),
        request_decode=getattr(
            request_converter_module, f"pdu_to_py_{request_name}"
        ),
        response_encode=lambda packet: bytes(response_encoder(packet)),
        response_decode=getattr(
            response_converter_module, f"pdu_to_py_{response_name}"
        ),
    )


class TypedRpcClient:
    """Registry-aware client that accepts and returns service body objects."""

    def __init__(
        self,
        rpc_client: RpcClient,
        service_name: str,
        service_type: str,
        *,
        package: str | None = None,
    ):
        self._rpc_client = rpc_client
        self.service_name = service_name
        self.service_type = service_type
        self.wire = load_service_wire(service_type, package)

    def create_request(self) -> Any:
        """Create an empty generated request body object."""
        return self.wire.request_packet_type().body

    def call(
        self,
        request: Any,
        timeout_usec: int,
        **kwargs: Any,
    ) -> Any:
        """Encode a request body and synchronously return the response body."""
        return self.call_async(request, timeout_usec, **kwargs).result()

    def call_async(
        self,
        request: Any,
        timeout_usec: int,
        **kwargs: Any,
    ) -> RpcFuture[Any]:
        """Encode a request body and return a Future for the response body.

        Packet conversion and request-ID validation remain owned by the typed
        adapter. Cancellation is forwarded to the underlying protocol-level
        RPC Future.
        """
        request_pdu, request_id = self._encode_request(request)
        raw_future = self._rpc_client.call_async(
            self.service_name,
            request_pdu,
            timeout_usec,
            **kwargs,
        )

        typed_future = RpcFuture[Any](raw_future.cancel)
        typed_future._set_running_or_notify_cancel()

        def complete(completed: RpcFuture[bytes]) -> None:
            try:
                response = self._decode_response(completed.result(), request_id)
            except BaseException as error:
                typed_future._set_exception(error)
            else:
                typed_future._set_result(response)

        raw_future.add_done_callback(complete)
        return typed_future

    def _encode_request(self, request: Any) -> tuple[bytes, Any]:
        base_pdu = self._rpc_client.create_request_buffer(self.service_name)
        request_packet = self.wire.request_decode(base_pdu)
        request_packet.body = request
        return self.wire.request_encode(request_packet), request_packet.header.request_id

    def _decode_response(self, response_pdu: bytes, request_id: Any) -> Any:
        response_packet = self.wire.response_decode(response_pdu)
        if response_packet.header.request_id != request_id:
            raise RuntimeError(
                "RPC response request_id mismatch: "
                f"expected={request_id}, actual={response_packet.header.request_id}"
            )
        status = int(response_packet.header.status)
        result_code = int(response_packet.header.result_code)
        if (
            status != int(RpcServiceStatus.DONE)
            or result_code != int(RpcServiceResultCode.OK)
        ):
            raise TypedRpcServiceError(
                self.service_name,
                status,
                result_code,
            )
        return response_packet.body


class TypedServerService:
    """Typed view of one Service managed by a TypedRpcServer."""

    def __init__(
        self,
        rpc_server: RpcServer | RpcMuxServer,
        service_name: str,
        service_type: str,
        wire: ServiceWire,
    ) -> None:
        self._rpc_server = rpc_server
        self.service_name = service_name
        self.service_type = service_type
        self.wire = wire

    def create_response(self) -> Any:
        return self.wire.response_packet_type().body

    def send_reply(
        self,
        request: TypedRpcServerPollResult,
        response: Any,
    ) -> None:
        self._validate_request(request, ServerEvent.REQUEST_IN)
        packet = self._create_response_packet(
            request,
            RpcServiceStatus.DONE,
            RpcServiceResultCode.OK,
        )
        packet.body = response
        self._rpc_server.send_reply(
            request.request_token,
            self.wire.response_encode(packet),
        )

    def send_error(
        self,
        request: TypedRpcServerPollResult,
        result_code: RpcServiceResultCode = RpcServiceResultCode.ERROR,
    ) -> None:
        self._validate_request(request, ServerEvent.REQUEST_IN)
        if result_code in {
            RpcServiceResultCode.OK,
            RpcServiceResultCode.CANCELED,
        }:
            raise ValueError(
                "error reply result_code must describe a non-cancel error"
            )
        packet = self._create_response_packet(
            request,
            RpcServiceStatus.ERROR,
            result_code,
        )
        self._rpc_server.send_reply(
            request.request_token,
            self.wire.response_encode(packet),
        )

    def send_cancel_reply(self, request: TypedRpcServerPollResult) -> None:
        self._validate_request(request, ServerEvent.REQUEST_CANCEL)
        packet = self._create_response_packet(
            request,
            RpcServiceStatus.DONE,
            RpcServiceResultCode.CANCELED,
        )
        self._rpc_server.send_cancel_reply(
            request.request_token,
            self.wire.response_encode(packet),
        )

    def _create_response_packet(
        self,
        request: TypedRpcServerPollResult,
        status: RpcServiceStatus,
        result_code: RpcServiceResultCode,
    ) -> Any:
        pdu = self._rpc_server.create_reply_buffer(
            request.request_token,
            int(status),
            int(result_code),
        )
        return self.wire.response_decode(pdu)

    def _validate_request(
        self,
        request: TypedRpcServerPollResult,
        expected_event: ServerEvent,
    ) -> None:
        if request.service_name != self.service_name:
            raise ValueError(
                "typed RPC request belongs to another Service: "
                f"expected={self.service_name!r}, "
                f"actual={request.service_name!r}"
            )
        if request.event != expected_event:
            raise ValueError(
                "typed RPC request has the wrong server event: "
                f"expected={expected_event.name}, actual={request.event.name}"
            )


class TypedRpcServer:
    """Registry-aware multi-Service adapter over a raw RPC server."""

    def __init__(
        self,
        rpc_server: RpcServer | RpcMuxServer,
        service_config_path: str | Path,
        *,
        packages: Mapping[str, str] | None = None,
    ) -> None:
        self._rpc_server = rpc_server
        definitions = _load_service_definitions(service_config_path)
        package_overrides = dict(packages or {})
        unknown_packages = package_overrides.keys() - definitions.keys()
        if unknown_packages:
            unknown = ", ".join(sorted(unknown_packages))
            raise ValueError(
                f"package override references unknown Service(s): {unknown}"
            )
        self._services = {
            service_name: TypedServerService(
                rpc_server,
                service_name,
                service_type,
                load_service_wire(
                    service_type,
                    package_overrides.get(service_name),
                ),
            )
            for service_name, service_type in definitions.items()
        }

    @property
    def service_names(self) -> tuple[str, ...]:
        return tuple(self._services)

    def service(self, service_name: str) -> TypedServerService:
        try:
            return self._services[service_name]
        except KeyError as error:
            raise KeyError(f"unknown Service: {service_name}") from error

    def poll(self) -> TypedRpcServerPollResult:
        raw = self._rpc_server.poll()
        request = TypedRpcServerPollResult(
            event=raw.event,
            service_name=raw.service_name,
            client_name=raw.client_name,
            request_token=raw.request_token,
        )
        if raw.event == ServerEvent.NONE:
            return request
        try:
            service = self._services[raw.service_name]
        except KeyError as error:
            raise RuntimeError(
                f"event references unknown Service: {raw.service_name!r}"
            ) from error
        if raw.event == ServerEvent.REQUEST_CANCEL:
            return request
        try:
            body = service.wire.request_decode(raw.pdu).body
        except BaseException as error:
            raise TypedRpcRequestDecodeError(request, error) from error
        return TypedRpcServerPollResult(
            event=raw.event,
            service_name=raw.service_name,
            client_name=raw.client_name,
            request_token=raw.request_token,
            request_body=body,
        )


def make_typed_client(
    rpc_client: RpcClient,
    service_name: str,
    service_type: str,
    *,
    package: str | None = None,
) -> TypedRpcClient:
    return TypedRpcClient(
        rpc_client,
        service_name,
        service_type,
        package=package,
    )


def make_typed_server(
    rpc_server: RpcServer | RpcMuxServer,
    service_config_path: str | Path,
    *,
    packages: Mapping[str, str] | None = None,
) -> TypedRpcServer:
    return TypedRpcServer(
        rpc_server,
        service_config_path,
        packages=packages,
    )


def _load_service_definitions(
    service_config_path: str | Path,
) -> dict[str, str]:
    path = Path(service_config_path)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(
            f"failed to load Service configuration: {path}"
        ) from error

    services = document.get("services") if isinstance(document, dict) else None
    if not isinstance(services, list) or not services:
        raise ValueError(
            "Service configuration must contain a non-empty services list"
        )

    definitions: dict[str, str] = {}
    for index, entry in enumerate(services):
        if not isinstance(entry, dict):
            raise ValueError(f"services[{index}] must be an object")
        service_name = entry.get("name")
        service_type = entry.get("type")
        if not isinstance(service_name, str) or not service_name:
            raise ValueError(
                f"services[{index}].name must be a non-empty string"
            )
        if not isinstance(service_type, str) or not service_type:
            raise ValueError(
                f"services[{index}].type must be a non-empty string"
            )
        _split_service_type(service_type)
        if service_name in definitions:
            raise ValueError(f"duplicate Service name: {service_name}")
        definitions[service_name] = service_type
    return definitions


def _split_service_type(service_type: str) -> tuple[str, str]:
    parts = service_type.split("/", 1)
    if len(parts) != 2 or not all(parts):
        raise ValueError(
            f"Service type must use package/Type form: {service_type}"
        )
    return parts[0], parts[1]
