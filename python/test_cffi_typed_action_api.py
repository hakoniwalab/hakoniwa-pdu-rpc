from __future__ import annotations

from types import SimpleNamespace

import pytest

from hakoniwa_pdu_rpc import (
    ActionClientEvent,
    ActionClientPollResult,
    ActionDecision,
    ActionTerminalStatus,
    ActionWire,
    ClientGoalHandle,
    TypedAction,
    TypedActionClient,
)


class RequestPacket:
    def __init__(self) -> None:
        self.header = SimpleNamespace(goal_id=bytes(16))
        self.body = SimpleNamespace()


class FakeActionClient:
    def __init__(self) -> None:
        self.sent = []
        self.canceled = []
        self.events = []

    def create_goal_buffer(self, action_name: str) -> bytes:
        return f"base-{action_name}".encode()

    def send_goal(
        self,
        action_name: str,
        pdu: bytes,
        goal_id: bytes,
        timeout_usec: int,
    ) -> ClientGoalHandle:
        self.sent.append((action_name, pdu, goal_id, timeout_usec))
        return ClientGoalHandle(goal_id)

    def cancel_goal(self, action_name: str, goal: ClientGoalHandle) -> None:
        self.canceled.append((action_name, goal))

    def poll(self) -> ActionClientPollResult:
        return self.events.pop(0)


def action_wire(action_name: str) -> ActionWire:
    def request_decode(pdu: bytes):
        assert pdu == f"base-{action_name}".encode()
        return SimpleNamespace(
            header=SimpleNamespace(goal_id=bytes(16)),
            body=None,
        )

    def request_encode(packet) -> bytes:
        assert packet.body.order == 10
        return f"encoded-{action_name}".encode()

    return ActionWire(
        request_packet_type=RequestPacket,
        response_packet_type=SimpleNamespace,
        feedback_packet_type=SimpleNamespace,
        request_encode=request_encode,
        request_decode=request_decode,
        response_encode=lambda _packet: b"unused",
        response_decode=lambda pdu: SimpleNamespace(
            body=SimpleNamespace(sequence=list(pdu), source=action_name)
        ),
        feedback_encode=lambda _packet: b"unused",
        feedback_decode=lambda pdu: SimpleNamespace(
            body=SimpleNamespace(sequence=list(pdu), source=action_name)
        ),
    )


def make_typed_client() -> tuple[TypedActionClient, FakeActionClient]:
    raw = FakeActionClient()
    client = TypedActionClient.__new__(TypedActionClient)
    client._action_client = raw
    client._actions = {
        name: TypedAction(raw, name, "sample_action_msgs/Fibonacci", action_wire(name))
        for name in ("fibonacci", "lucas")
    }
    return client, raw


def event(
    event_type: ActionClientEvent,
    *,
    pdu: bytes = b"",
    action_name: str = "fibonacci",
    status: ActionTerminalStatus = ActionTerminalStatus.UNSPECIFIED,
) -> ActionClientPollResult:
    return ActionClientPollResult(
        event=event_type,
        action_name=action_name,
        goal=None if event_type == ActionClientEvent.NONE else ClientGoalHandle(bytes(range(1, 17))),
        decision=ActionDecision.UNSPECIFIED,
        terminal_status=status,
        feedback_sequence=0,
        pdu=pdu,
    )


def test_creates_typed_goal_body() -> None:
    client, _ = make_typed_client()
    goal = client.action("fibonacci").create_goal()
    goal.order = 10
    assert goal.order == 10


def test_encodes_typed_goal_through_raw_action_client() -> None:
    client, raw = make_typed_client()
    goal_id = bytes(range(1, 17))
    goal = SimpleNamespace(order=10)

    handle = client.action("fibonacci").send_goal(
        goal, goal_id, timeout_usec=3_000_000
    )

    assert handle.goal_id == goal_id
    assert raw.sent == [
        ("fibonacci", b"encoded-fibonacci", goal_id, 3_000_000)
    ]


def test_decodes_only_feedback_and_result_bodies() -> None:
    client, raw = make_typed_client()
    raw.events.extend(
        [
            event(ActionClientEvent.GOAL_RESPONSE, pdu=b"not-decoded"),
            event(ActionClientEvent.FEEDBACK, pdu=b"\x01\x02"),
            event(
                ActionClientEvent.RESULT,
                pdu=b"\x03\x05",
                status=ActionTerminalStatus.SUCCEEDED,
            ),
        ]
    )

    goal_response = client.poll()
    feedback = client.poll()
    result = client.poll()

    assert goal_response.feedback is None
    assert goal_response.result is None
    assert feedback.feedback.sequence == [1, 2]
    assert feedback.feedback.source == "fibonacci"
    assert feedback.result is None
    assert result.feedback is None
    assert result.result.sequence == [3, 5]
    assert result.terminal_status == ActionTerminalStatus.SUCCEEDED


@pytest.mark.parametrize(
    "event_type",
    [
        ActionClientEvent.NONE,
        ActionClientEvent.GOAL_RESPONSE,
        ActionClientEvent.CANCEL_RESPONSE,
        ActionClientEvent.TIMEOUT,
        ActionClientEvent.ERROR,
    ],
)
def test_non_payload_events_do_not_synthesize_bodies(event_type) -> None:
    client, raw = make_typed_client()
    raw.events.append(event(event_type, pdu=b"ignored"))

    incoming = client.poll()

    assert incoming.feedback is None
    assert incoming.result is None


def test_cancel_preserves_typed_goal_handle() -> None:
    client, raw = make_typed_client()
    handle = ClientGoalHandle(bytes(range(1, 17)))

    client.action("fibonacci").cancel_goal(handle)

    assert raw.canceled == [("fibonacci", handle)]


def test_routes_events_from_multiple_actions() -> None:
    client, raw = make_typed_client()
    raw.events.extend(
        [
            event(
                ActionClientEvent.FEEDBACK,
                action_name="fibonacci",
                pdu=b"\x01",
            ),
            event(
                ActionClientEvent.RESULT,
                action_name="lucas",
                pdu=b"\x02",
            ),
        ]
    )

    first = client.poll()
    second = client.poll()

    assert first.feedback.source == "fibonacci"
    assert second.result.source == "lucas"


def test_rejects_event_from_unknown_action() -> None:
    client, raw = make_typed_client()
    raw.events.append(
        event(ActionClientEvent.GOAL_RESPONSE, action_name="unknown-action")
    )

    with pytest.raises(RuntimeError, match="event references unknown Action"):
        client.poll()


def test_exposes_configured_action_names() -> None:
    client, _ = make_typed_client()

    assert client.action_names == ("fibonacci", "lucas")
    with pytest.raises(KeyError, match="unknown Action"):
        client.action("unknown")
