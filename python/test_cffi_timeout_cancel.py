from __future__ import annotations

import os
import time
from pathlib import Path

from hakoniwa_pdu_rpc.cffi_api import ClientEvent, RpcClient, RpcServer, ServerEvent


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
    raise AssertionError("server event timed out")


def wait_client(client: RpcClient, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = client.poll()
        if result.event != ClientEvent.NONE:
            return result
        time.sleep(0.001)
    raise AssertionError("client event timed out")


def call_when_connected(
    client: RpcClient, request: bytes, timeout_usec: int, timeout: float = 3.0
) -> None:
    deadline = time.monotonic() + timeout
    while True:
        try:
            client.call(SERVICE, request, timeout_usec=timeout_usec)
            return
        except Exception:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def test_timeout_requires_explicit_cancel_and_endpoint_remains_reusable():
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
        call_when_connected(client, request, timeout_usec=50_000)

        original = wait_server(server)
        assert original.event == ServerEvent.REQUEST_IN
        assert original.service_name == SERVICE
        assert isinstance(original.pdu, bytes)

        timeout = wait_client(client)
        assert timeout.event == ClientEvent.RESPONSE_TIMEOUT
        assert timeout.service_name == SERVICE

        client.cancel(SERVICE)

        cancel = wait_server(server)
        assert cancel.event == ServerEvent.REQUEST_CANCEL
        assert cancel.service_name == SERVICE
        assert isinstance(cancel.pdu, bytes)

        cancel_reply = server.create_reply_buffer(
            cancel.request_token,
            status=STATUS_DONE,
            result_code=RESULT_CANCELED,
        )
        server.send_cancel_reply(cancel.request_token, cancel_reply)

        canceled = wait_client(client)
        assert canceled.event == ClientEvent.RESPONSE_CANCEL
        assert canceled.service_name == SERVICE
        assert isinstance(canceled.pdu, bytes)

        next_request = client.create_request_buffer(SERVICE)
        call_when_connected(client, next_request, timeout_usec=1_000_000)

        next_incoming = wait_server(server)
        assert next_incoming.event == ServerEvent.REQUEST_IN

        next_reply = server.create_reply_buffer(
            next_incoming.request_token,
            status=STATUS_DONE,
            result_code=RESULT_OK,
        )
        server.send_reply(next_incoming.request_token, next_reply)

        response = wait_client(client)
        assert response.event == ClientEvent.RESPONSE_IN
        assert response.service_name == SERVICE
        assert isinstance(response.pdu, bytes)
