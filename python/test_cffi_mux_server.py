from __future__ import annotations

import os
import sys
import time
from pathlib import Path

from hakoniwa_pdu_rpc import (
    CffiRpcClient,
    ClientEvent,
    RpcMuxServer,
    ServerEvent,
    load_service_wire,
)


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "hakoniwa-pdu-registry"))

SERVICE_CONFIG = ROOT / "test" / "configs" / "service_config_mux.json"
CLIENT_ENDPOINT_CONFIG = ROOT / "test" / "configs" / "endpoints_mux_clients.json"
MUX_ENDPOINT_CONFIG = ROOT / "test" / "configs" / "mux_server_endpoint.json"
SERVICE = "Service/Add"
STATUS_DONE = 3
RESULT_OK = 0


def wait_connected(server: RpcMuxServer, expected: int, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        server.poll()
        if server.connected_count() == expected:
            return
        time.sleep(0.001)
    raise AssertionError(
        f"mux connection timed out: {server.connected_count()} of {expected}"
    )


def wait_request(server: RpcMuxServer, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        incoming = server.poll()
        if incoming.event == ServerEvent.REQUEST_IN:
            return incoming
        time.sleep(0.001)
    raise AssertionError("mux server request timed out")


def wait_response(client: CffiRpcClient, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = client.poll()
        if response.event == ClientEvent.RESPONSE_IN:
            return response
        time.sleep(0.001)
    raise AssertionError("RPC client response timed out")


def call_with_retry(
    client: CffiRpcClient,
    request_pdu: bytes,
    timeout: float = 3.0,
) -> None:
    deadline = time.monotonic() + timeout
    while True:
        try:
            client.call(SERVICE, request_pdu, timeout_usec=1_000_000)
            return
        except Exception:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def create_request(client: CffiRpcClient, wire, a: int, b: int) -> bytes:
    request_pdu = client.create_request_buffer(SERVICE)
    packet = wire.request_decode(request_pdu)
    packet.body.a = a
    packet.body.b = b
    return wire.request_encode(packet)


def reply(server: RpcMuxServer, wire, incoming, expected_a: int, expected_b: int) -> None:
    request = wire.request_decode(incoming.pdu)
    assert request.body.a == expected_a
    assert request.body.b == expected_b

    reply_pdu = server.create_reply_buffer(
        incoming.request_token,
        status=STATUS_DONE,
        result_code=RESULT_OK,
    )
    response = wire.response_decode(reply_pdu)
    response.body.sum = request.body.a + request.body.b
    server.send_reply(
        incoming.request_token,
        wire.response_encode(response),
    )


def assert_response(client: CffiRpcClient, wire, expected_sum: int) -> None:
    result = wait_response(client)
    assert result.service_name == SERVICE
    response = wire.response_decode(result.pdu)
    assert response.body.sum == expected_sum


def test_python_mux_server_routes_two_clients_on_one_listener() -> None:
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    wire = load_service_wire("AddTwoInts")

    server = RpcMuxServer(
        library,
        "server_node",
        SERVICE_CONFIG,
        MUX_ENDPOINT_CONFIG,
    )
    client0 = CffiRpcClient(
        library,
        "client_node",
        "TestClient0",
        SERVICE_CONFIG,
        CLIENT_ENDPOINT_CONFIG,
    )
    client1 = CffiRpcClient(
        library,
        "client_node",
        "TestClient1",
        SERVICE_CONFIG,
        CLIENT_ENDPOINT_CONFIG,
    )

    started = False
    try:
        server.start()
        client0.start()
        client1.start()
        started = True

        wait_connected(server, 2)
        assert server.expected_count() == 2
        assert server.connected_count() == 2
        assert server.is_ready()

        call_with_retry(client0, create_request(client0, wire, 10, 20))
        call_with_retry(client1, create_request(client1, wire, 40, 2))

        requests = {}
        for _ in range(2):
            incoming = wait_request(server)
            requests[incoming.client_name] = incoming

        assert set(requests) == {"TestClient0", "TestClient1"}
        reply(server, wire, requests["TestClient0"], 10, 20)
        reply(server, wire, requests["TestClient1"], 40, 2)
        assert_response(client0, wire, 30)
        assert_response(client1, wire, 42)

        call_with_retry(client0, create_request(client0, wire, 7, 8))
        reused = wait_request(server)
        assert reused.client_name == "TestClient0"
        reply(server, wire, reused, 7, 8)
        assert_response(client0, wire, 15)
    finally:
        if started:
            server.stop()
        client0.close()
        client1.close()
        server.close()
