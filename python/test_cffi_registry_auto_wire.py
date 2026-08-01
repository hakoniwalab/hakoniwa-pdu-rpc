from __future__ import annotations

import os
import sys
import threading
import time
from pathlib import Path

from hakoniwa_pdu_rpc import (
    RpcClient,
    RpcServer,
    ServerEvent,
    load_service_wire,
    make_typed_client,
)


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "hakoniwa-pdu-registry"))

SERVICE_CONFIG = ROOT / "test" / "configs" / "service_config.json"
ENDPOINT_CONFIG = ROOT / "test" / "configs" / "endpoints.json"
SERVICE = "Service/Add"
STATUS_DONE = 3
RESULT_OK = 0


def wait_request(server: RpcServer, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        incoming = server.poll()
        if incoming.event == ServerEvent.REQUEST_IN:
            return incoming
        time.sleep(0.001)
    raise AssertionError("server request timed out")


def test_registry_auto_wire_round_trip_returns_generated_response_body():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    wire = load_service_wire("AddTwoInts")

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
    ) as rpc_client:
        server.start()
        rpc_client.start()
        client = make_typed_client(
            rpc_client,
            SERVICE,
            "AddTwoInts",
        )

        errors: list[BaseException] = []

        def serve_once() -> None:
            try:
                incoming = wait_request(server)
                request_packet = wire.request_decode(incoming.pdu)
                assert request_packet.body.a == 10
                assert request_packet.body.b == 20

                reply_pdu = server.create_reply_buffer(
                    incoming.request_token,
                    status=STATUS_DONE,
                    result_code=RESULT_OK,
                )
                response_packet = wire.response_decode(reply_pdu)
                response_packet.body.sum = (
                    request_packet.body.a + request_packet.body.b
                )
                server.send_reply(
                    incoming.request_token,
                    wire.response_encode(response_packet),
                )
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=serve_once)
        worker.start()
        try:
            request = client.create_request()
            request.a = 10
            request.b = 20
            response = client.call(request, timeout_usec=1_000_000)
            assert response.sum == 30
        finally:
            worker.join(timeout=5.0)

        assert not worker.is_alive()
        assert not errors
