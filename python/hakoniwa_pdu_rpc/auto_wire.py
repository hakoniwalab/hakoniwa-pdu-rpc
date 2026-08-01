from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
from typing import Any, Callable

from .client import RpcClient


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
    package: str = "pdu.python.hako_srv_msgs",
) -> ServiceWire:
    """Load generated PDU Registry packet types and converters by convention."""
    request_name = f"{service_type}RequestPacket"
    response_name = f"{service_type}ResponsePacket"

    try:
        request_type_module = import_module(
            f"{package}.pdu_pytype_{request_name}"
        )
        response_type_module = import_module(
            f"{package}.pdu_pytype_{response_name}"
        )
        request_converter_module = import_module(
            f"{package}.pdu_conv_{request_name}"
        )
        response_converter_module = import_module(
            f"{package}.pdu_conv_{response_name}"
        )

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
    except (ImportError, AttributeError) as error:
        raise RuntimeError(
            "failed to load generated PDU Registry service components: "
            f"service_type={service_type!r}, package={package!r}"
        ) from error


class TypedRpcClient:
    """Registry-aware client that accepts and returns service body objects."""

    def __init__(
        self,
        rpc_client: RpcClient,
        service_name: str,
        service_type: str,
        *,
        package: str = "pdu.python.hako_srv_msgs",
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
        """Encode a request body, perform RPC, and return the response body."""
        base_pdu = self._rpc_client.create_request_buffer(self.service_name)
        request_packet = self.wire.request_decode(base_pdu)
        request_packet.body = request
        request_id = request_packet.header.request_id

        response_pdu = self._rpc_client.call(
            self.service_name,
            self.wire.request_encode(request_packet),
            timeout_usec,
            **kwargs,
        )
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
    package: str = "pdu.python.hako_srv_msgs",
) -> TypedRpcClient:
    return TypedRpcClient(
        rpc_client,
        service_name,
        service_type,
        package=package,
    )
