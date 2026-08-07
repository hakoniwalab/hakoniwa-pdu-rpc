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
        assert action_name == "fibonacci"
        return b"base-goal"

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


def make_typed_client() -> tuple[TypedActionClient, FakeActionClient]:
    raw = FakeActionClient()

    def request_decode(pdu: bytes):
        assert pdu == b"base-goal"
        return SimpleNamespace(
            header=SimpleNamespace(goal_id=bytes(16)),
            body=None,
        )

    def request_encode(packet) -> bytes:
        assert packet.body.order == 10
        return b"encoded-goal"

    client = TypedActionClient.__new__(TypedActionClient)
    client._action_client = raw
    client.action_name = "fibonacci"
    client.action_type = "sample_action_msgs/Fibonacci"
    client.wire = ActionWire(
        request_packet_type=RequestPacket,
        response_packet_type=SimpleNamespace,
        feedback_packet_type=SimpleNamespace,
        request_encode=request_encode,
        request_decode=request_decode,
        response_encode=lambda _packet: b"unused",
        response_decode=lambda pdu: SimpleNamespace(
            body=SimpleNamespace(sequence=list(pdu))
        ),
        feedback_encode=lambda _packet: b"unused",
        feedback_decode=lambda pdu: SimpleNamespace(
            body=SimpleNamespace(sequence=list(pdu))
        ),
    )
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
    goal = client.create_goal()
    goal.order = 10
    assert goal.order == 10


def test_encodes_typed_goal_through_raw_action_client() -> None:
    client, raw = make_typed_client()
    goal_id = bytes(range(1, 17))
    goal = SimpleNamespace(order=10)

    handle = client.send_goal(goal, goal_id, timeout_usec=3_000_000)

    assert handle.goal_id == goal_id
    assert raw.sent == [
        ("fibonacci", b"encoded-goal", goal_id, 3_000_000)
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

    client.cancel_goal(handle)

    assert raw.canceled == [("fibonacci", handle)]


def test_rejects_event_from_another_action() -> None:
    client, raw = make_typed_client()
    raw.events.append(
        event(ActionClientEvent.GOAL_RESPONSE, action_name="another-action")
    )

    with pytest.raises(RuntimeError, match="Action event name mismatch"):
        client.poll()
