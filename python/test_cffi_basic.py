from __future__ import annotations

import os
import time
from pathlib import Path

from hakoniwa_pdu_rpc.cffi_api import ClientEvent, RpcClient, RpcServer, ServerEvent


ROOT = Path(__file__).resolve().parents[1]
SERVICE_CONFIG = ROOT / "test" / "configs" / "service_config.json"
ENDPOINT_CONFIG = ROOT / "test" / "configs" / "endpoints.json"
SERVICE = "Service/Add"


def wait_server(server: RpcServer, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = server.poll()
        if result.event != ServerEvent.NONE:
            return result
        time.sleep(0.001)
    raise AssertionError("server request timed out")


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

        # The Python smoke test validates the ownership boundary rather than
        # generated message conversion. A default success reply is sufficient.
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
