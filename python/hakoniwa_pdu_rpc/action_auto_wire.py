from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
import json
from pathlib import Path
from typing import Any, Callable, Mapping

from .action_cffi import (
    ActionClient,
    ActionClientEvent,
    ActionDecision,
    ActionMuxServer,
    ActionServer,
    ActionServerEvent,
    ActionTerminalStatus,
    ClientGoalHandle,
    RuntimeCancelCause,
    ServerGoalHandle,
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


@dataclass(frozen=True)
class TypedActionServerPollResult:
    event: ActionServerEvent
    action_name: str
    goal: ServerGoalHandle | None
    runtime_cancel_cause: RuntimeCancelCause
    goal_body: Any | None = None


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
        request_encode=lambda packet: bytes(request_encoder(packet)),
        request_decode=getattr(
            request_converter_module, f"pdu_to_py_{request_name}"
        ),
        response_encode=lambda packet: bytes(response_encoder(packet)),
        response_decode=getattr(
            response_converter_module, f"pdu_to_py_{response_name}"
        ),
        feedback_encode=lambda packet: bytes(feedback_encoder(packet)),
        feedback_decode=getattr(
            feedback_converter_module, f"pdu_to_py_{feedback_name}"
        ),
    )


class TypedAction:
    """Typed view of one Action managed by a TypedActionClient."""

    def __init__(
        self,
        action_client: ActionClient,
        action_name: str,
        action_type: str,
        wire: ActionWire,
    ) -> None:
        self._action_client = action_client
        self.action_name = action_name
        self.action_type = action_type
        self.wire = wire

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


class TypedActionClient:
    """Registry-aware multi-Action adapter over one raw ActionClient.

    Callers exchange Action body objects. Packet headers, converters and raw
    wire buffers remain owned by this adapter and the native endpoint. Polling
    is centralized here because the underlying client returns events for every
    Action in its resolved configuration.
    """

    def __init__(
        self,
        action_client: ActionClient,
        action_config_path: str | Path,
        *,
        packages: Mapping[str, str] | None = None,
    ) -> None:
        self._action_client = action_client
        definitions = _load_action_definitions(action_config_path)
        package_overrides = dict(packages or {})
        unknown_packages = package_overrides.keys() - definitions.keys()
        if unknown_packages:
            unknown = ", ".join(sorted(unknown_packages))
            raise ValueError(
                f"package override references unknown Action(s): {unknown}"
            )
        self._actions = {
            action_name: TypedAction(
                action_client,
                action_name,
                action_type,
                load_action_wire(
                    action_type,
                    package_overrides.get(action_name),
                ),
            )
            for action_name, action_type in definitions.items()
        }

    @property
    def action_names(self) -> tuple[str, ...]:
        return tuple(self._actions)

    def action(self, action_name: str) -> TypedAction:
        try:
            return self._actions[action_name]
        except KeyError as error:
            raise KeyError(f"unknown Action: {action_name}") from error

    def poll(self) -> TypedActionClientPollResult:
        raw = self._action_client.poll()
        feedback = None
        result = None
        if raw.event != ActionClientEvent.NONE:
            try:
                action = self._actions[raw.action_name]
            except KeyError as error:
                raise RuntimeError(
                    f"event references unknown Action: {raw.action_name!r}"
                ) from error
            if raw.event == ActionClientEvent.FEEDBACK:
                feedback = action.wire.feedback_decode(raw.pdu).body
            elif raw.event == ActionClientEvent.RESULT:
                result = action.wire.response_decode(raw.pdu).body

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


class TypedServerAction:
    """Typed view of one Action managed by a TypedActionServer."""

    def __init__(
        self,
        action_server: ActionServer | ActionMuxServer,
        action_name: str,
        action_type: str,
        wire: ActionWire,
    ) -> None:
        self._action_server = action_server
        self.action_name = action_name
        self.action_type = action_type
        self.wire = wire

    def accept_goal(self, goal: ServerGoalHandle) -> None:
        self._action_server.accept_goal(self.action_name, goal)

    def reject_goal(self, goal: ServerGoalHandle) -> None:
        self._action_server.reject_goal(self.action_name, goal)

    def accept_cancel(self, goal: ServerGoalHandle) -> None:
        self._action_server.accept_cancel(self.action_name, goal)

    def reject_cancel(self, goal: ServerGoalHandle) -> None:
        self._action_server.reject_cancel(self.action_name, goal)

    def create_feedback(self) -> Any:
        return self.wire.feedback_packet_type().body

    def send_feedback(
        self,
        goal: ServerGoalHandle,
        feedback: Any,
    ) -> None:
        base_pdu = self._action_server.create_feedback_buffer(self.action_name)
        packet = self.wire.feedback_decode(base_pdu)
        packet.body = feedback
        self._action_server.send_feedback(
            self.action_name,
            goal,
            self.wire.feedback_encode(packet),
        )

    def create_result(self) -> Any:
        return self.wire.response_packet_type().body

    def complete(
        self,
        goal: ServerGoalHandle,
        status: ActionTerminalStatus,
        result: Any,
    ) -> None:
        base_pdu = self._action_server.create_result_buffer(self.action_name)
        packet = self.wire.response_decode(base_pdu)
        packet.body = result
        self._action_server.complete(
            self.action_name,
            goal,
            status,
            self.wire.response_encode(packet),
        )


class TypedActionServer:
    """Registry-aware multi-Action adapter over one raw Action server."""

    def __init__(
        self,
        action_server: ActionServer | ActionMuxServer,
        action_config_path: str | Path,
        *,
        packages: Mapping[str, str] | None = None,
    ) -> None:
        self._action_server = action_server
        definitions = _load_action_definitions(action_config_path)
        package_overrides = dict(packages or {})
        unknown_packages = package_overrides.keys() - definitions.keys()
        if unknown_packages:
            unknown = ", ".join(sorted(unknown_packages))
            raise ValueError(
                f"package override references unknown Action(s): {unknown}"
            )
        self._actions = {
            action_name: TypedServerAction(
                action_server,
                action_name,
                action_type,
                load_action_wire(
                    action_type,
                    package_overrides.get(action_name),
                ),
            )
            for action_name, action_type in definitions.items()
        }

    @property
    def action_names(self) -> tuple[str, ...]:
        return tuple(self._actions)

    def action(self, action_name: str) -> TypedServerAction:
        try:
            return self._actions[action_name]
        except KeyError as error:
            raise KeyError(f"unknown Action: {action_name}") from error

    def poll(self) -> TypedActionServerPollResult:
        raw = self._action_server.poll()
        goal_body = None
        if raw.event != ActionServerEvent.NONE:
            try:
                action = self._actions[raw.action_name]
            except KeyError as error:
                raise RuntimeError(
                    f"event references unknown Action: {raw.action_name!r}"
                ) from error
            if raw.event == ActionServerEvent.GOAL_REQUEST:
                goal_body = action.wire.request_decode(raw.pdu).body

        return TypedActionServerPollResult(
            event=raw.event,
            action_name=raw.action_name,
            goal=raw.goal,
            runtime_cancel_cause=raw.runtime_cancel_cause,
            goal_body=goal_body,
        )


def make_typed_action_client(
    action_client: ActionClient,
    action_config_path: str | Path,
    *,
    packages: Mapping[str, str] | None = None,
) -> TypedActionClient:
    return TypedActionClient(
        action_client,
        action_config_path,
        packages=packages,
    )


def make_typed_action_server(
    action_server: ActionServer | ActionMuxServer,
    action_config_path: str | Path,
    *,
    packages: Mapping[str, str] | None = None,
) -> TypedActionServer:
    return TypedActionServer(
        action_server,
        action_config_path,
        packages=packages,
    )


def _load_action_definitions(
    action_config_path: str | Path,
) -> dict[str, str]:
    path = Path(action_config_path)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"failed to load Action configuration: {path}") from error

    actions = document.get("actions") if isinstance(document, dict) else None
    if not isinstance(actions, list) or not actions:
        raise ValueError("Action configuration must contain a non-empty actions list")

    definitions: dict[str, str] = {}
    for index, entry in enumerate(actions):
        if not isinstance(entry, dict):
            raise ValueError(f"actions[{index}] must be an object")
        action_name = entry.get("name")
        action_type = entry.get("type")
        if not isinstance(action_name, str) or not action_name:
            raise ValueError(f"actions[{index}].name must be a non-empty string")
        if not isinstance(action_type, str) or not action_type:
            raise ValueError(f"actions[{index}].type must be a non-empty string")
        _split_action_type(action_type)
        if action_name in definitions:
            raise ValueError(f"duplicate Action name: {action_name}")
        definitions[action_name] = action_type
    return definitions


def _split_action_type(action_type: str) -> tuple[str, str]:
    parts = action_type.split("/", 1)
    if len(parts) != 2 or not all(parts):
        raise ValueError(
            f"Action type must use package/Type form: {action_type}"
        )
    return parts[0], parts[1]
