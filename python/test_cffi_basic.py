from __future__ import annotations

import os
import threading
import time
from pathlib import Path

import pytest

from hakoniwa_pdu_rpc import RpcClient as HighLevelRpcClient, RpcTimeoutError
from hakoniwa_pdu_rpc.cffi_api import ClientEvent, RpcClient, RpcServer, ServerEvent
from test_cffi_registry_auto_wire import (
    test_registry_auto_wire_round_trip_returns_generated_response_body,
)
from test_cffi_timeout_cancel import (
    test_timeout_requires_explicit_cancel_and_endpoint_remains_reusable,
)


ROOT = Path(__file__).resolve().parents[1]
SERVICE_CONFIG = ROOT / "test" / "configs" / "service_config.json"
ENDPOINT_CONFIG = ROOT / "test" / "configs" / "endpoints.json"
SERVICE = "Service/Add"
STATUS_DONE = 3
RESULT_OK = 0
RESULT_CANCELED = 2


def wait_server(server: RpcServer, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = server.poll()
        if result.event != ServerEvent.NONE:
            return result
        time.sleep(0.001)
    raise AssertionError("server request timed out")


def wait_server_event(server: RpcServer, expected: ServerEvent, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = server.poll()
        if result.event == expected:
            return result
        time.sleep(0.001)
    raise AssertionError(f"server event timed out: {expected}")


def wait_client(client: RpcClient, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = client.poll()
        if result.event != ClientEvent.NONE:
            return result
        time.sleep(0.001)
    raise AssertionError("client response timed out")


def test_basic_round_trip_copies_and_frees_native_buffers():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]

    with RpcServer(
        library,
        "server_node",
        SERVICE_CONFIG,
        ENDPOINT_CONFIG,
    ) as server, RpcClient(
        library,
        "client_node",
        "TestClient",
        SERVICE_CONFIG,
        ENDPOINT_CONFIG,
    ) as client:
        server.start()
        client.start()

        request = client.create_request_buffer(SERVICE)
        assert isinstance(request, bytes)
        assert request

        deadline = time.monotonic() + 3.0
        while True:
            try:
                client.call(SERVICE, request, timeout_usec=1_000_000)
                break
            except Exception:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.01)

        incoming = wait_server(server)
        assert incoming.event == ServerEvent.REQUEST_IN
        assert incoming.service_name == SERVICE
        assert isinstance(incoming.pdu, bytes)

        reply = server.create_reply_buffer(
            incoming.request_token,
            status=0,
            result_code=0,
        )
        assert isinstance(reply, bytes)
        server.send_reply(incoming.request_token, reply)

        response = wait_client(client)
        assert response.event == ClientEvent.RESPONSE_IN
        assert response.service_name == SERVICE
        assert isinstance(response.pdu, bytes)


def start_high_level_pair(library: str):
    server = RpcServer(
        library,
        "server_node",
        SERVICE_CONFIG,
        ENDPOINT_CONFIG,
    )
    client = HighLevelRpcClient(
        library,
        "client_node",
        "TestClient",
        SERVICE_CONFIG,
        ENDPOINT_CONFIG,
    )
    server.start()
    client.start()
    return server, client


def test_high_level_call_returns_python_bytes():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    server, client = start_high_level_pair(library)
    errors: list[BaseException] = []

    def serve_once():
        try:
            incoming = wait_server_event(server, ServerEvent.REQUEST_IN)
            reply = server.create_reply_buffer(
                incoming.request_token,
                status=STATUS_DONE,
                result_code=RESULT_OK,
            )
            server.send_reply(incoming.request_token, reply)
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=serve_once)
    worker.start()
    try:
        request = client.create_request_buffer(SERVICE)
        response = client.call(SERVICE, request, timeout_usec=1_000_000)
        assert isinstance(response, bytes)
        assert response
    finally:
        worker.join(timeout=5.0)
        client.close()
        server.close()

    assert not worker.is_alive()
    assert not errors


def test_high_level_call_auto_cancels_after_timeout():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    server, client = start_high_level_pair(library)
    errors: list[BaseException] = []

    def serve_cancel():
        try:
            wait_server_event(server, ServerEvent.REQUEST_IN)
            cancel = wait_server_event(server, ServerEvent.REQUEST_CANCEL)
            reply = server.create_reply_buffer(
                cancel.request_token,
                status=STATUS_DONE,
                result_code=RESULT_CANCELED,
            )
            server.send_cancel_reply(cancel.request_token, reply)
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=serve_cancel)
    worker.start()
    try:
        request = client.create_request_buffer(SERVICE)
        with pytest.raises(RpcTimeoutError, match="timed out and was canceled"):
            client.call(SERVICE, request, timeout_usec=50_000)
    finally:
        worker.join(timeout=5.0)
        client.close()
        server.close()

    assert not worker.is_alive()
    assert not errors


def test_high_level_call_returns_normal_response_when_it_wins_cancel_race():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    server, client = start_high_level_pair(library)
    errors: list[BaseException] = []

    def serve_race():
        try:
            incoming = wait_server_event(server, ServerEvent.REQUEST_IN)
            time.sleep(0.08)
            reply = server.create_reply_buffer(
                incoming.request_token,
                status=STATUS_DONE,
                result_code=RESULT_OK,
            )
            server.send_reply(incoming.request_token, reply)
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=serve_race)
    worker.start()
    try:
        request = client.create_request_buffer(SERVICE)
        response = client.call(SERVICE, request, timeout_usec=50_000)
        assert isinstance(response, bytes)
        assert response
    finally:
        worker.join(timeout=5.0)
        client.close()
        server.close()

    assert not worker.is_alive()
    assert not errors
