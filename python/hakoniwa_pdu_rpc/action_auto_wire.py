from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
import struct
from typing import Any, Callable

from .action_cffi import (
    ActionClient,
    ActionClientEvent,
    ActionDecision,
    ActionTerminalStatus,
    ClientGoalHandle,
)


@dataclass(frozen=True)
class ActionWire:
    request_packet_type: type
    response_packet_type: type
    feedback_packet_type: type
    request_encode: Callable[[Any], bytes]
    request_decode: Callable[[bytes], Any]
    response_encode: Callable[[Any], bytes]
    response_decode: Callable[[bytes], Any]
    feedback_encode: Callable[[Any], bytes]
    feedback_decode: Callable[[bytes], Any]


@dataclass(frozen=True)
class TypedActionClientPollResult:
    event: ActionClientEvent
    action_name: str
    goal: ClientGoalHandle | None
    decision: ActionDecision
    terminal_status: ActionTerminalStatus
    feedback_sequence: int
    feedback: Any | None = None
    result: Any | None = None


def load_action_wire(
    action_type: str,
    package: str | None = None,
) -> ActionWire:
    """Load generated Action packet types and converters by convention.

    ``action_type`` uses ``package/Type`` form.  When ``package`` is omitted,
    the installed hakoniwa-pdu layout is tried before the Registry source-tree
    layout used by this repository's tests.
    """

    package_name, type_name = _split_action_type(action_type)
    packages = (
        (package,)
        if package is not None
        else (
            f"hakoniwa_pdu.pdu_msgs.{package_name}",
            f"pdu.python.{package_name}",
        )
    )
    errors: list[BaseException] = []
    for candidate in packages:
        try:
            return _load_action_wire_from_package(type_name, candidate)
        except (ImportError, AttributeError) as error:
            errors.append(error)

    attempted = ", ".join(repr(candidate) for candidate in packages)
    raise RuntimeError(
        "failed to load generated PDU Registry Action components: "
        f"action_type={action_type!r}, packages=[{attempted}]"
    ) from errors[-1]


def _load_action_wire_from_package(
    action_type: str,
    package: str,
) -> ActionWire:
    request_name = f"{action_type}ActionRequest"
    response_name = f"{action_type}ActionResponse"
    feedback_name = f"{action_type}ActionFeedback"

    request_type_module = import_module(f"{package}.pdu_pytype_{request_name}")
    response_type_module = import_module(f"{package}.pdu_pytype_{response_name}")
    feedback_type_module = import_module(f"{package}.pdu_pytype_{feedback_name}")
    request_converter_module = import_module(f"{package}.pdu_conv_{request_name}")
    response_converter_module = import_module(f"{package}.pdu_conv_{response_name}")
    feedback_converter_module = import_module(f"{package}.pdu_conv_{feedback_name}")

    request_encoder = getattr(
        request_converter_module, f"py_to_pdu_{request_name}"
    )
    response_encoder = getattr(
        response_converter_module, f"py_to_pdu_{response_name}"
    )
    feedback_encoder = getattr(
        feedback_converter_module, f"py_to_pdu_{feedback_name}"
    )
    return ActionWire(
        request_packet_type=getattr(request_type_module, request_name),
        response_packet_type=getattr(response_type_module, response_name),
        feedback_packet_type=getattr(feedback_type_module, feedback_name),
        request_encode=lambda packet: _canonicalize_pdu_layout(request_encoder(packet)),
        request_decode=getattr(
            request_converter_module, f"pdu_to_py_{request_name}"
        ),
        response_encode=lambda packet: _canonicalize_pdu_layout(response_encoder(packet)),
        response_decode=getattr(
            response_converter_module, f"pdu_to_py_{response_name}"
        ),
        feedback_encode=lambda packet: _canonicalize_pdu_layout(feedback_encoder(packet)),
        feedback_decode=getattr(
            feedback_converter_module, f"pdu_to_py_{feedback_name}"
        ),
    )


class TypedActionClient:
    """Registry-aware adapter over a raw ActionClient.

    Callers exchange Action body objects. Packet headers, converters and raw
    wire buffers remain owned by this adapter and the native endpoint.
    """

    def __init__(
        self,
        action_client: ActionClient,
        action_name: str,
        action_type: str,
        *,
        package: str | None = None,
    ) -> None:
        self._action_client = action_client
        self.action_name = action_name
        self.action_type = action_type
        self.wire = load_action_wire(action_type, package)

    def create_goal(self) -> Any:
        return self.wire.request_packet_type().body

    def send_goal(
        self,
        goal: Any,
        goal_id: bytes,
        timeout_usec: int = 0,
    ) -> ClientGoalHandle:
        base_pdu = self._action_client.create_goal_buffer(self.action_name)
        request_packet = self.wire.request_decode(base_pdu)
        request_packet.body = goal
        request_pdu = self.wire.request_encode(request_packet)
        return self._action_client.send_goal(
            self.action_name,
            request_pdu,
            goal_id,
            timeout_usec,
        )

    def cancel_goal(self, goal: ClientGoalHandle) -> None:
        self._action_client.cancel_goal(self.action_name, goal)

    def poll(self) -> TypedActionClientPollResult:
        raw = self._action_client.poll()
        if raw.event != ActionClientEvent.NONE and raw.action_name != self.action_name:
            raise RuntimeError(
                "Action event name mismatch: "
                f"expected={self.action_name!r}, actual={raw.action_name!r}"
            )

        feedback = None
        result = None
        if raw.event == ActionClientEvent.FEEDBACK:
            feedback = self.wire.feedback_decode(raw.pdu).body
        elif raw.event == ActionClientEvent.RESULT:
            result = self.wire.response_decode(raw.pdu).body

        return TypedActionClientPollResult(
            event=raw.event,
            action_name=raw.action_name,
            goal=raw.goal,
            decision=raw.decision,
            terminal_status=raw.terminal_status,
            feedback_sequence=raw.feedback_sequence,
            feedback=feedback,
            result=result,
        )


def make_typed_action_client(
    action_client: ActionClient,
    action_name: str,
    action_type: str,
    *,
    package: str | None = None,
) -> TypedActionClient:
    return TypedActionClient(
        action_client,
        action_name,
        action_type,
        package=package,
    )


def _split_action_type(action_type: str) -> tuple[str, str]:
    parts = action_type.split("/", 1)
    if len(parts) != 2 or not all(parts):
        raise ValueError(
            f"Action type must use package/Type form: {action_type}"
        )
    return parts[0], parts[1]


def _canonicalize_pdu_layout(encoded: bytes | bytearray) -> bytes:
    """Align the generated Python PDU base/heap boundary to the native ABI.

    Some generated converters emit the heap immediately after an unaligned
    base. Native Action endpoints use the canonical eight-byte-aligned PDU
    layout. Heap references are relative to ``heap_off``, so relocating the
    heap while preserving base and heap contents is lossless.
    """

    data = bytes(encoded)
    metadata_size = 24
    if len(data) < metadata_size:
        raise ValueError("generated Action PDU is smaller than its metadata")
    magic, version, base_off, heap_off, total_size = struct.unpack_from(
        "<IIIII", data, 0
    )
    if (
        magic != 0x12345678
        or version != 1
        or base_off != metadata_size
        or not base_off <= heap_off <= total_size <= len(data)
    ):
        raise ValueError("generated Action PDU has invalid metadata")

    base = data[base_off:heap_off]
    heap = data[heap_off:total_size]
    canonical_heap_off = metadata_size + ((len(base) + 7) & ~7)
    canonical_total_size = canonical_heap_off + len(heap)
    if heap_off == canonical_heap_off and total_size == len(data):
        return data

    canonical = bytearray(canonical_total_size)
    canonical[:metadata_size] = data[:metadata_size]
    struct.pack_into(
        "<IIIII",
        canonical,
        0,
        magic,
        version,
        base_off,
        canonical_heap_off,
        canonical_total_size,
    )
    canonical[base_off : base_off + len(base)] = base
    canonical[canonical_heap_off:] = heap
    return bytes(canonical)
