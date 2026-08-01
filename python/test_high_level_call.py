from __future__ import annotations

import os
import threading
import time
from pathlib import Path

import pytest

from hakoniwa_pdu_rpc import RpcClient, RpcServer, RpcTimeoutError, ServerEvent


ROOT = Path(__file__).resolve().parents[1]
SERVICE_CONFIG = ROOT / "test" / "configs" / "service_config.json"
ENDPOINT_CONFIG = ROOT / "test" / "configs" / "endpoints.json"
SERVICE = "Service/Add"
STATUS_DONE = 3
RESULT_OK = 0
RESULT_CANCELED = 2


def wait_server(server: RpcServer, expected: ServerEvent, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = server.poll()
        if result.event == expected:
            return result
        time.sleep(0.001)
    raise AssertionError(f"server event timed out: {expected}")


def start_pair(library: str):
    server = RpcServer(
        library,
        "server_node",
        SERVICE_CONFIG,
        ENDPOINT_CONFIG,
    )
    client = RpcClient(
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
    server, client = start_pair(library)
    errors: list[BaseException] = []

    def serve_once():
        try:
            incoming = wait_server(server, ServerEvent.REQUEST_IN)
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
    server, client = start_pair(library)
    errors: list[BaseException] = []

    def serve_cancel():
        try:
            wait_server(server, ServerEvent.REQUEST_IN)
            cancel = wait_server(server, ServerEvent.REQUEST_CANCEL)
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
    server, client = start_pair(library)
    errors: list[BaseException] = []

    def serve_race():
        try:
            incoming = wait_server(server, ServerEvent.REQUEST_IN)
            # Let the client observe timeout and queue cancel, but complete the
            # original request before polling REQUEST_CANCEL.
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
