from __future__ import annotations

import sys
import struct
from pathlib import Path

import pytest

from hakoniwa_pdu_rpc import load_action_wire


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "hakoniwa-pdu-registry"))


def test_loads_fibonacci_action_wire_from_registry_layout() -> None:
    wire = load_action_wire("sample_action_msgs/Fibonacci")

    request = wire.request_packet_type()
    request.body.order = 10
    encoded = wire.request_encode(request)
    decoded = wire.request_decode(encoded)

    assert decoded.body.order == 10
    response = wire.response_packet_type()
    response.body.sequence = [0, 1, 1]
    encoded_response = wire.response_encode(response)
    _, _, _, heap_off, total_size = struct.unpack_from(
        "<IIIII", encoded_response, 0
    )
    assert heap_off % 8 == 0
    assert total_size == len(encoded_response)
    assert list(wire.response_decode(encoded_response).body.sequence) == [0, 1, 1]
    feedback = wire.feedback_packet_type()
    feedback.body.partial_sequence = [1, 2, 3]
    assert list(
        wire.feedback_decode(wire.feedback_encode(feedback)).body.partial_sequence
    ) == [1, 2, 3]
    assert wire.response_packet_type.__name__ == "FibonacciActionResponse"
    assert wire.feedback_packet_type.__name__ == "FibonacciActionFeedback"


def test_rejects_action_type_without_package() -> None:
    with pytest.raises(ValueError, match="package/Type"):
        load_action_wire("Fibonacci")
