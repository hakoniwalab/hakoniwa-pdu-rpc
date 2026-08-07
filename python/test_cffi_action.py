from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

from hakoniwa_pdu_rpc import (
    ActionClient,
    ActionClientEvent,
    ActionDecision,
    ActionError,
    ActionErrorCode,
    ActionServer,
    ActionServerEvent,
    ActionTerminalStatus,
    ClientGoalHandle,
)


ROOT = Path(__file__).resolve().parents[1]
ACTION_CONFIG = ROOT / "test" / "configs" / "action_resolved.json"
ENDPOINT_CONFIG = ROOT / "test" / "configs" / "action_c_tcp_e2e" / "endpoints.json"
ACTION_NAME = "fibonacci"


def goal_id(seed: int) -> bytes:
    return bytes((seed + index) & 0xFF for index in range(16))


def wait_running(client: ActionClient, server: ActionServer) -> None:
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if client.is_running() and server.is_running():
            return
        time.sleep(0.001)
    raise AssertionError("Action TCP endpoints did not become ready")


def wait_server(server: ActionServer, expected: ActionServerEvent):
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


class ActionPair:
    def __init__(self):
        library = os.environ["HAKO_PDU_RPC_LIBRARY"]
        self.server = ActionServer(
            library,
            "fibonacci-server",
            ACTION_CONFIG,
            ENDPOINT_CONFIG,
        )
        self.client = ActionClient(
            library,
            "fibonacci-client",
            "python-action-cffi-test",
            ACTION_CONFIG,
            ENDPOINT_CONFIG,
        )

    def __enter__(self):
        self.server.start()
        self.client.start()
        wait_running(self.client, self.server)
        return self

    def __exit__(self, *_args):
        self.client.close()
        self.server.close()


def test_action_goal_feedback_and_result_round_trip():
    with ActionPair() as runtime:
        request = runtime.client.create_goal_buffer(ACTION_NAME)
        goal = runtime.client.send_goal(
            ACTION_NAME, request, goal_id(0x10), timeout_usec=1_000_000
        )

        incoming = wait_server(runtime.server, ActionServerEvent.GOAL_REQUEST)
        assert incoming.goal is not None
        assert incoming.goal.goal_id == goal.goal_id
        assert isinstance(incoming.pdu, bytes)

        runtime.server.accept_goal(ACTION_NAME, incoming.goal)
        response = wait_client(runtime.client, ActionClientEvent.GOAL_RESPONSE)
        assert response.goal == goal
        assert response.decision == ActionDecision.ACCEPTED

        feedback = runtime.server.create_feedback_buffer(ACTION_NAME)
        runtime.server.send_feedback(ACTION_NAME, incoming.goal, feedback)
        delivered_feedback = wait_client(runtime.client, ActionClientEvent.FEEDBACK)
        assert delivered_feedback.goal == goal
        assert delivered_feedback.feedback_sequence == 0
        assert isinstance(delivered_feedback.pdu, bytes)

        result = runtime.server.create_result_buffer(ACTION_NAME)
        runtime.server.complete(
            ACTION_NAME,
            incoming.goal,
            ActionTerminalStatus.SUCCEEDED,
            result,
        )
        delivered_result = wait_client(runtime.client, ActionClientEvent.RESULT)
        assert delivered_result.goal == goal
        assert delivered_result.terminal_status == ActionTerminalStatus.SUCCEEDED
        assert isinstance(delivered_result.pdu, bytes)


def test_action_cancel_round_trip():
    with ActionPair() as runtime:
        request = runtime.client.create_goal_buffer(ACTION_NAME)
        goal = runtime.client.send_goal(ACTION_NAME, request, goal_id(0x40))
        incoming = wait_server(runtime.server, ActionServerEvent.GOAL_REQUEST)
        assert incoming.goal is not None

        runtime.server.accept_goal(ACTION_NAME, incoming.goal)
        accepted = wait_client(runtime.client, ActionClientEvent.GOAL_RESPONSE)
        assert accepted.decision == ActionDecision.ACCEPTED

        runtime.client.cancel_goal(ACTION_NAME, goal)
        cancel = wait_server(runtime.server, ActionServerEvent.CANCEL_REQUEST)
        assert cancel.goal == incoming.goal
        runtime.server.accept_cancel(ACTION_NAME, cancel.goal)
        cancel_response = wait_client(
            runtime.client, ActionClientEvent.CANCEL_RESPONSE
        )
        assert cancel_response.decision == ActionDecision.ACCEPTED

        result = runtime.server.create_result_buffer(ACTION_NAME)
        runtime.server.complete(
            ACTION_NAME,
            incoming.goal,
            ActionTerminalStatus.CANCELED,
            result,
        )
        canceled = wait_client(runtime.client, ActionClientEvent.RESULT)
        assert canceled.terminal_status == ActionTerminalStatus.CANCELED


def test_action_send_goal_preserves_native_error_codes():
    with ActionPair() as runtime:
        request = runtime.client.create_goal_buffer(ACTION_NAME)
        first_id = goal_id(0x70)
        runtime.client.send_goal(ACTION_NAME, request, first_id)

        with pytest.raises(ActionError) as duplicate:
            runtime.client.send_goal(ACTION_NAME, request, first_id)
        assert duplicate.value.code == ActionErrorCode.DUPLICATE_GOAL

        # The shared Action fixture has four slots. Leave all Goals pending so
        # the fifth submission is rejected before transport send.
        runtime.client.send_goal(ACTION_NAME, request, goal_id(0x80))
        runtime.client.send_goal(ACTION_NAME, request, goal_id(0x90))
        runtime.client.send_goal(ACTION_NAME, request, goal_id(0xA0))
        with pytest.raises(ActionError) as exhausted:
            runtime.client.send_goal(ACTION_NAME, request, goal_id(0xB0))
        assert exhausted.value.code == ActionErrorCode.NO_FREE_SLOT


def test_action_goal_handle_normalizes_and_validates_identity():
    mutable_id = bytearray(goal_id(0xC0))
    goal = ClientGoalHandle(mutable_id)
    mutable_id[0] = 0
    assert isinstance(goal.goal_id, bytes)
    assert goal.goal_id[0] == 0xC0

    with pytest.raises(ValueError, match="not all-zero"):
        ClientGoalHandle(bytes(16))
