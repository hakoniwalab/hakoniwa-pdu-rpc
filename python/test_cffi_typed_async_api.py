from __future__ import annotations

import threading
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any

import pytest

from hakoniwa_pdu_rpc import (
    RpcFuture,
    ServiceWire,
    TypedRpcClient,
    TypedRpcServiceError,
)


@dataclass
class Header:
    request_id: int
    status: int = 3
    result_code: int = 0


@dataclass
class Packet:
    header: Header
    body: Any


class RequestPacket:
    def __init__(self) -> None:
        self.body = SimpleNamespace()


class FakeRpcClient:
    def __init__(self) -> None:
        self.cancel_requested = threading.Event()
        self.raw_future = RpcFuture[bytes](self._cancel)
        self.raw_future._set_running_or_notify_cancel()
        self.calls: list[tuple[str, bytes, int, dict[str, Any]]] = []

    def _cancel(self) -> None:
        self.cancel_requested.set()

    def create_request_buffer(self, service_name: str) -> bytes:
        assert service_name == "Service/Add"
        return b"base-request"

    def call_async(
        self,
        service_name: str,
        pdu: bytes,
        timeout_usec: int,
        **kwargs: Any,
    ) -> RpcFuture[bytes]:
        self.calls.append((service_name, pdu, timeout_usec, kwargs))
        return self.raw_future


def make_typed_client() -> tuple[TypedRpcClient, FakeRpcClient]:
    rpc_client = FakeRpcClient()

    def request_decode(pdu: bytes) -> Packet:
        assert pdu == b"base-request"
        return Packet(header=Header(request_id=17), body=None)

    def request_encode(packet: Packet) -> bytes:
        assert packet.header.request_id == 17
        assert packet.body.a == 10
        assert packet.body.b == 20
        return b"encoded-request"

    def response_decode(pdu: bytes) -> Packet:
        request_id = 99 if pdu == b"mismatched-response" else 17
        status = 4 if pdu == b"error-response" else 3
        result_code = 3 if pdu == b"error-response" else 0
        return Packet(
            header=Header(
                request_id=request_id,
                status=status,
                result_code=result_code,
            ),
            body=SimpleNamespace(sum=30),
        )

    client = TypedRpcClient.__new__(TypedRpcClient)
    client._rpc_client = rpc_client
    client.service_name = "Service/Add"
    client.service_type = "AddTwoInts"
    client.wire = ServiceWire(
        request_packet_type=RequestPacket,
        response_packet_type=Packet,
        request_encode=request_encode,
        request_decode=request_decode,
        response_encode=lambda packet: b"unused",
        response_decode=response_decode,
    )
    return client, rpc_client


def make_request() -> SimpleNamespace:
    return SimpleNamespace(a=10, b=20)


def test_typed_call_async_returns_typed_response_body() -> None:
    client, rpc_client = make_typed_client()

    future = client.call_async(
        make_request(),
        timeout_usec=1_000_000,
        event_timeout_sec=2.0,
    )

    assert isinstance(future, RpcFuture)
    assert future.running()
    assert rpc_client.calls == [
        (
            "Service/Add",
            b"encoded-request",
            1_000_000,
            {"event_timeout_sec": 2.0},
        )
    ]

    rpc_client.raw_future._set_result(b"response")
    response = future.result(timeout=1.0)
    assert response.sum == 30


def test_typed_call_uses_the_same_async_conversion_path() -> None:
    client, rpc_client = make_typed_client()
    rpc_client.raw_future._set_result(b"response")

    response = client.call(make_request(), timeout_usec=1_000_000)

    assert response.sum == 30


def test_typed_call_async_propagates_request_id_mismatch() -> None:
    client, rpc_client = make_typed_client()
    future = client.call_async(make_request(), timeout_usec=1_000_000)

    rpc_client.raw_future._set_result(b"mismatched-response")

    with pytest.raises(RuntimeError, match="request_id mismatch"):
        future.result(timeout=1.0)


def test_typed_call_async_propagates_raw_rpc_exception() -> None:
    client, rpc_client = make_typed_client()
    future = client.call_async(make_request(), timeout_usec=1_000_000)

    rpc_client.raw_future._set_exception(RuntimeError("transport failed"))

    with pytest.raises(RuntimeError, match="transport failed"):
        future.result(timeout=1.0)


def test_typed_call_async_exposes_service_error_status() -> None:
    client, rpc_client = make_typed_client()
    future = client.call_async(make_request(), timeout_usec=1_000_000)

    rpc_client.raw_future._set_result(b"error-response")

    with pytest.raises(TypedRpcServiceError) as caught:
        future.result(timeout=1.0)
    assert caught.value.service_name == "Service/Add"
    assert caught.value.status == 4
    assert caught.value.result_code == 3


def test_typed_future_cancel_forwards_protocol_cancel() -> None:
    client, rpc_client = make_typed_client()
    future = client.call_async(make_request(), timeout_usec=1_000_000)

    assert future.cancel()
    assert rpc_client.cancel_requested.wait(timeout=1.0)
