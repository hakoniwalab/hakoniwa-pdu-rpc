from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
from typing import Any, Callable

from .client import RpcClient
from .future import RpcFuture


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


def load_service_wire(
    service_type: str,
    package: str | None = None,
) -> ServiceWire:
    """Load generated PDU Registry packet types and converters by convention.

    When ``package`` is omitted, prefer the installed ``hakoniwa-pdu`` package
    layout used by hakoniwa-pdu-ros, then fall back to the Registry source-tree
    layout used by this repository's submodule tests.
    """
    packages = (package,) if package is not None else DEFAULT_SERVICE_PACKAGES
    errors: list[BaseException] = []

    for candidate in packages:
        try:
            return _load_service_wire_from_package(service_type, candidate)
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
        return response_packet.body


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
