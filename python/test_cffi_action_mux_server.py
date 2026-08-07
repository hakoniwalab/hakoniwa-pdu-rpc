from __future__ import annotations

import os
import time
from pathlib import Path

from hakoniwa_pdu_rpc import (
    ActionClient,
    ActionClientEvent,
    ActionDecision,
    ActionMuxServer,
    ActionServerEvent,
    ActionTerminalStatus,
    RuntimeCancelCause,
)


ROOT = Path(__file__).resolve().parents[1]
ACTION_CONFIG = ROOT / "test" / "configs" / "action_resolved.json"
CLIENT_ENDPOINT_CONFIG = (
    ROOT / "test" / "configs" / "action_mux_e2e" / "endpoints.json"
)
SERVER_ENDPOINT_CONFIG = (
    ROOT / "test" / "configs" / "action_mux_e2e" / "server_endpoint.json"
)
ACTION_NAME = "fibonacci"


def goal_id(seed: int) -> bytes:
    return bytes((seed + index) & 0xFF for index in range(16))


def wait_ready(server: ActionMuxServer) -> None:
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        result = server.poll()
        assert result.event == ActionServerEvent.NONE
        if server.is_ready():
            return
        time.sleep(0.001)
    raise AssertionError("Action Mux Server did not accept both clients")


def wait_server(server: ActionMuxServer, expected: ActionServerEvent):
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        result = server.poll()
        if result.event == expected:
            return result
        assert result.event == ActionServerEvent.NONE
        time.sleep(0.001)
    raise AssertionError(f"server event timed out: {expected.name}")


def wait_client(client: ActionClient, expected: ActionClientEvent):
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        result = client.poll()
        if result.event == expected:
            return result
        assert result.event == ActionClientEvent.NONE
        time.sleep(0.001)
    raise AssertionError(f"client event timed out: {expected.name}")


def test_action_mux_server_routes_two_clients_with_shared_goal_handle_contract():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    server = ActionMuxServer(
        library,
        "fibonacci-server",
        ACTION_CONFIG,
        SERVER_ENDPOINT_CONFIG,
    )
    clients = [
        ActionClient(
            library,
            "fibonacci-client",
            f"python-action-mux-client-{index}",
            ACTION_CONFIG,
            CLIENT_ENDPOINT_CONFIG,
        )
        for index in range(2)
    ]

    try:
        server.start()
        for client in clients:
            client.start()
        wait_ready(server)
        assert server.connected_count() == 2
        assert server.expected_count() == 2

        server_goals = []
        client_goals = []
        for index, client in enumerate(clients):
            request = client.create_goal_buffer(ACTION_NAME)
            client_goal = client.send_goal(
                ACTION_NAME,
                request,
                goal_id(0x20 + index * 0x20),
                timeout_usec=1_000_000,
            )
            incoming = wait_server(server, ActionServerEvent.GOAL_REQUEST)
            assert incoming.goal is not None
            assert incoming.goal.goal_id == client_goal.goal_id
            server.accept_goal(incoming.action_name, incoming.goal)
            accepted = wait_client(client, ActionClientEvent.GOAL_RESPONSE)
            assert accepted.decision == ActionDecision.ACCEPTED
            server_goals.append(incoming.goal)
            client_goals.append(client_goal)

        feedback = server.create_feedback_buffer(ACTION_NAME)
        server.send_feedback(ACTION_NAME, server_goals[0], feedback)
        delivered_feedback = wait_client(clients[0], ActionClientEvent.FEEDBACK)
        assert delivered_feedback.goal == client_goals[0]

        clients[1].cancel_goal(ACTION_NAME, client_goals[1])
        cancel = wait_server(server, ActionServerEvent.CANCEL_REQUEST)
        assert cancel.goal == server_goals[1]
        server.accept_cancel(cancel.action_name, cancel.goal)
        cancel_response = wait_client(
            clients[1], ActionClientEvent.CANCEL_RESPONSE
        )
        assert cancel_response.decision == ActionDecision.ACCEPTED

        for index, client in enumerate(clients):
            status = (
                ActionTerminalStatus.SUCCEEDED
                if index == 0
                else ActionTerminalStatus.CANCELED
            )
            result = server.create_result_buffer(ACTION_NAME)
            server.complete(ACTION_NAME, server_goals[index], status, result)
            delivered = wait_client(client, ActionClientEvent.RESULT)
            assert delivered.goal == client_goals[index]
            assert delivered.terminal_status == status

        request = clients[0].create_goal_buffer(ACTION_NAME)
        disconnected_goal = clients[0].send_goal(
            ACTION_NAME,
            request,
            goal_id(0x70),
            timeout_usec=1_000_000,
        )
        incoming = wait_server(server, ActionServerEvent.GOAL_REQUEST)
        assert incoming.goal is not None
        server.accept_goal(incoming.action_name, incoming.goal)
        accepted = wait_client(clients[0], ActionClientEvent.GOAL_RESPONSE)
        assert accepted.goal == disconnected_goal

        clients[0].close()
        runtime_cancel = wait_server(
            server, ActionServerEvent.RUNTIME_CANCEL_REQUEST
        )
        assert runtime_cancel.goal == incoming.goal
        assert (
            runtime_cancel.runtime_cancel_cause
            == RuntimeCancelCause.TRANSPORT_DISCONNECTED
        )
        server.accept_cancel(runtime_cancel.action_name, runtime_cancel.goal)
        local_result = server.create_result_buffer(ACTION_NAME)
        server.complete(
            runtime_cancel.action_name,
            runtime_cancel.goal,
            ActionTerminalStatus.CANCELED,
            local_result,
        )
        assert server.connected_count() == 1
    finally:
        for client in clients:
            client.close()
        server.close()
