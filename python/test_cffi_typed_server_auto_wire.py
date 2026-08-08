"""Contract tests for the typed multi-Service server adapter."""

from __future__ import annotations

import json
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any

import pytest

import hakoniwa_pdu_rpc.auto_wire as auto_wire
from hakoniwa_pdu_rpc import (
    RpcServiceResultCode,
    RpcServiceStatus,
    ServerEvent,
    ServerPollResult,
    ServiceWire,
    TypedRpcRequestDecodeError,
    TypedRpcServer,
)


@dataclass
class Header:
    request_id: int = 17
    status: int = int(RpcServiceStatus.NONE)
    result_code: int = int(RpcServiceResultCode.OK)


@dataclass
class Packet:
    header: Header
    body: Any


class EmptyPacket:
    def __init__(self) -> None:
        self.header = Header()
        self.body = SimpleNamespace()


class FakeRawServer:
    def __init__(self) -> None:
        self.events: list[ServerPollResult] = []
        self.created: list[tuple[int, int, int]] = []
        self.replies: list[tuple[int, bytes]] = []
        self.cancel_replies: list[tuple[int, bytes]] = []

    def poll(self) -> ServerPollResult:
        if self.events:
            return self.events.pop(0)
        return ServerPollResult(ServerEvent.NONE, 0, "", "", b"")

    def create_reply_buffer(
        self,
        request_token: int,
        status: int,
        result_code: int,
    ) -> Packet:
        self.created.append((request_token, status, result_code))
        return Packet(
            Header(request_id=request_token, status=status, result_code=result_code),
            SimpleNamespace(),
        )

    def send_reply(self, request_token: int, pdu: bytes) -> None:
        self.replies.append((request_token, pdu))

    def send_cancel_reply(self, request_token: int, pdu: bytes) -> None:
        self.cancel_replies.append((request_token, pdu))


def make_wire(service_type: str) -> ServiceWire:
    def request_decode(pdu: bytes) -> Packet:
        if pdu == b"broken":
            raise ValueError("broken request")
        return Packet(Header(), SimpleNamespace(value=pdu.decode("ascii")))

    def response_encode(packet: Packet) -> bytes:
        value = getattr(packet.body, "value", "")
        return (
            f"{packet.header.status}:{packet.header.result_code}:{value}"
        ).encode("ascii")

    return ServiceWire(
        request_packet_type=EmptyPacket,
        response_packet_type=EmptyPacket,
        request_encode=lambda packet: b"unused",
        request_decode=request_decode,
        response_encode=response_encode,
        response_decode=lambda packet: packet,
    )


@pytest.fixture
def service_config(tmp_path):
    path = tmp_path / "services.json"
    path.write_text(
        json.dumps(
            {
                "services": [
                    {"name": "Service/Add", "type": "pkg/Add"},
                    {"name": "Service/Subtract", "type": "pkg/Subtract"},
                ]
            }
        ),
        encoding="utf-8",
    )
    return path


@pytest.fixture
def typed_server(service_config, monkeypatch):
    raw = FakeRawServer()
    loaded: list[tuple[str, str | None]] = []

    def load_wire(service_type: str, package: str | None = None) -> ServiceWire:
        loaded.append((service_type, package))
        return make_wire(service_type)

    monkeypatch.setattr(auto_wire, "load_service_wire", load_wire)
    server = TypedRpcServer(
        raw,
        service_config,
        packages={"Service/Subtract": "custom.generated"},
    )
    assert loaded == [
        ("pkg/Add", None),
        ("pkg/Subtract", "custom.generated"),
    ]
    return server, raw


def test_typed_server_routes_multiple_services_and_decodes_request(typed_server):
    server, raw = typed_server
    raw.events.append(
        ServerPollResult(
            ServerEvent.REQUEST_IN,
            41,
            "Service/Subtract",
            "Client0",
            b"request-body",
        )
    )

    request = server.poll()

    assert server.service_names == ("Service/Add", "Service/Subtract")
    assert request.service_name == "Service/Subtract"
    assert request.client_name == "Client0"
    assert request.request_token == 41
    assert request.request_body.value == "request-body"


def test_typed_server_sends_normal_and_error_replies(typed_server):
    server, raw = typed_server
    request = auto_wire.TypedRpcServerPollResult(
        ServerEvent.REQUEST_IN,
        "Service/Add",
        "Client0",
        42,
        SimpleNamespace(value="request"),
    )
    service = server.service("Service/Add")
    response = service.create_response()
    response.value = "response"

    service.send_reply(request, response)
    service.send_error(request, RpcServiceResultCode.INVALID)

    assert raw.created == [
        (42, int(RpcServiceStatus.DONE), int(RpcServiceResultCode.OK)),
        (42, int(RpcServiceStatus.ERROR), int(RpcServiceResultCode.INVALID)),
    ]
    assert raw.replies == [
        (42, b"3:0:response"),
        (42, b"4:3:"),
    ]


def test_typed_server_sends_cancel_reply_with_done_canceled(typed_server):
    server, raw = typed_server
    request = auto_wire.TypedRpcServerPollResult(
        ServerEvent.REQUEST_CANCEL,
        "Service/Add",
        "Client0",
        43,
    )

    server.service("Service/Add").send_cancel_reply(request)

    assert raw.created == [
        (43, int(RpcServiceStatus.DONE), int(RpcServiceResultCode.CANCELED))
    ]
    assert raw.cancel_replies == [(43, b"3:2:")]


def test_typed_server_rejects_reply_api_for_wrong_event(typed_server):
    server, _raw = typed_server
    cancel = auto_wire.TypedRpcServerPollResult(
        ServerEvent.REQUEST_CANCEL,
        "Service/Add",
        "Client0",
        44,
    )

    with pytest.raises(ValueError, match="expected=REQUEST_IN"):
        server.service("Service/Add").send_error(cancel)


def test_typed_server_decode_error_keeps_request_metadata(typed_server):
    server, raw = typed_server
    raw.events.append(
        ServerPollResult(
            ServerEvent.REQUEST_IN,
            45,
            "Service/Add",
            "Client0",
            b"broken",
        )
    )

    with pytest.raises(TypedRpcRequestDecodeError) as caught:
        server.poll()

    assert caught.value.request.service_name == "Service/Add"
    assert caught.value.request.request_token == 45


def test_typed_server_returns_cancel_without_decoding_body(typed_server):
    server, raw = typed_server
    raw.events.append(
        ServerPollResult(
            ServerEvent.REQUEST_CANCEL,
            46,
            "Service/Add",
            "Client0",
            b"broken",
        )
    )

    request = server.poll()

    assert request.event == ServerEvent.REQUEST_CANCEL
    assert request.request_body is None
