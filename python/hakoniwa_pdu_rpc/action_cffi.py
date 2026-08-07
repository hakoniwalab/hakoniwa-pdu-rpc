from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path

from .cffi_api import _borrow_bytes, _get_binding


class ActionErrorCode(IntEnum):
    OK = 0
    INVALID_ARGUMENT = 1
    INITIALIZE = 2
    START = 3
    NOT_RUNNING = 4
    SEND = 5
    BUFFER_TOO_SMALL = 6
    NOT_FOUND = 7
    INVALID_STATE = 8
    DUPLICATE_GOAL = 9
    NO_FREE_SLOT = 10
    INVALID_PACKET = 11
    INTERNAL = 12


class ActionError(RuntimeError):
    def __init__(self, code: int | ActionErrorCode):
        self.code = ActionErrorCode(int(code))
        super().__init__(f"native Action error: {self.code.name} ({int(self.code)})")


class ActionDecision(IntEnum):
    UNSPECIFIED = 0
    ACCEPTED = 1
    REJECTED = 2


class ActionTerminalStatus(IntEnum):
    UNSPECIFIED = 0
    SUCCEEDED = 1
    CANCELED = 2
    ABORTED = 3


class ActionClientEvent(IntEnum):
    NONE = 0
    GOAL_RESPONSE = 1
    FEEDBACK = 2
    CANCEL_RESPONSE = 3
    RESULT = 4
    TIMEOUT = 5
    ERROR = 6


class ActionServerEvent(IntEnum):
    NONE = 0
    GOAL_REQUEST = 1
    CANCEL_REQUEST = 2
    RUNTIME_CANCEL_REQUEST = 3
    ERROR = 4


class RuntimeCancelCause(IntEnum):
    UNSPECIFIED = 0
    TRANSPORT_DISCONNECTED = 1
    SERVER_SHUTDOWN = 2
    INTERNAL_POLICY = 3


@dataclass(frozen=True)
class ClientGoalHandle:
    goal_id: bytes

    def __post_init__(self) -> None:
        object.__setattr__(self, "goal_id", _validate_goal_id(self.goal_id))


@dataclass(frozen=True)
class ServerGoalHandle:
    goal_id: bytes

    def __post_init__(self) -> None:
        object.__setattr__(self, "goal_id", _validate_goal_id(self.goal_id))


@dataclass(frozen=True)
class ActionClientPollResult:
    event: ActionClientEvent
    action_name: str
    goal: ClientGoalHandle | None
    decision: ActionDecision
    terminal_status: ActionTerminalStatus
    feedback_sequence: int
    pdu: bytes


@dataclass(frozen=True)
class ActionServerPollResult:
    event: ActionServerEvent
    action_name: str
    goal: ServerGoalHandle | None
    runtime_cancel_cause: RuntimeCancelCause
    pdu: bytes


def _validate_goal_id(goal_id: bytes) -> bytes:
    value = bytes(goal_id)
    if len(value) != 16 or not any(value):
        raise ValueError("Action Goal ID must be 16 bytes and not all-zero")
    return value


class _ActionBinding:
    def __init__(self, library_path: str | Path):
        shared = _get_binding(library_path)
        self.ffi = shared.ffi
        self.lib = shared.lib

    def encode(self, value: str | Path):
        return self.ffi.new("char[]", str(value).encode("utf-8"))

    def check(self, error: int) -> None:
        if int(error) != int(ActionErrorCode.OK):
            raise ActionError(int(error))

    def allocated_call(self, function, *args) -> bytes:
        out_buffer = self.ffi.new("uint8_t **")
        out_size = self.ffi.new("size_t *")
        error = int(function(*args, out_buffer, out_size))
        pointer = out_buffer[0]
        try:
            self.check(error)
            return b"" if pointer == self.ffi.NULL else bytes(
                self.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            self.lib.hako_pdu_action_buffer_free(pointer)

    def goal_id(self, value: bytes):
        native = self.ffi.new("hako_pdu_action_goal_id_t *")
        self.ffi.memmove(native.bytes, _validate_goal_id(value), 16)
        return native

    def client_goal(self, goal: ClientGoalHandle):
        native = self.ffi.new("hako_pdu_action_client_goal_handle_t *")
        self.ffi.memmove(native.goal_id.bytes, goal.goal_id, 16)
        return native

    def server_goal(self, goal: ServerGoalHandle):
        native = self.ffi.new("hako_pdu_action_server_goal_handle_t *")
        self.ffi.memmove(native.goal_id.bytes, goal.goal_id, 16)
        return native

    def goal_bytes(self, native) -> bytes:
        return bytes(self.ffi.buffer(native.goal_id.bytes, 16))
class ActionClient:
    def __init__(
        self,
        library_path: str | Path,
        node_id: str,
        client_name: str,
        action_config_path: str | Path,
        endpoint_config_path: str | Path,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
    ):
        self._binding = _ActionBinding(library_path)
        b = self._binding
        self._handle = b.lib.hako_pdu_action_client_create(
            b.encode(node_id),
            b.encode(client_name),
            b.encode(action_config_path),
            b.encode(endpoint_config_path),
            delta_time_usec,
            b.encode(time_source_type),
        )
        if self._handle == b.ffi.NULL:
            raise ActionError(ActionErrorCode.INITIALIZE)
        self._closed = False

    def start(self) -> None:
        self._binding.check(
            self._binding.lib.hako_pdu_action_client_start(self._handle)
        )

    def is_running(self) -> bool:
        b = self._binding
        out_running = b.ffi.new("int *")
        b.check(b.lib.hako_pdu_action_client_is_running(self._handle, out_running))
        return bool(out_running[0])

    def create_goal_buffer(self, action_name: str) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_action_client_create_goal_buffer_alloc,
            self._handle,
            b.encode(action_name),
        )

    def send_goal(
        self,
        action_name: str,
        pdu: bytes,
        goal_id: bytes,
        timeout_usec: int = 0,
    ) -> ClientGoalHandle:
        b = self._binding
        native_goal_id = b.goal_id(goal_id)
        out_goal = b.ffi.new("hako_pdu_action_client_goal_handle_t *")
        native_pdu = _borrow_bytes(b, pdu)
        b.check(
            b.lib.hako_pdu_action_client_send_goal(
                self._handle,
                b.encode(action_name),
                native_pdu,
                len(pdu),
                native_goal_id,
                out_goal,
                timeout_usec,
            )
        )
        return ClientGoalHandle(b.goal_bytes(out_goal))

    def cancel_goal(self, action_name: str, goal: ClientGoalHandle) -> None:
        b = self._binding
        native_goal = b.client_goal(goal)
        b.check(
            b.lib.hako_pdu_action_client_cancel_goal(
                self._handle, b.encode(action_name), native_goal
            )
        )

    def poll(self) -> ActionClientPollResult:
        b = self._binding
        info = b.ffi.new("hako_pdu_action_client_event_info_t *")
        out_buffer = b.ffi.new("uint8_t **")
        out_size = b.ffi.new("size_t *")
        out_error = b.ffi.new("hako_pdu_action_error_t *")
        event = ActionClientEvent(
            int(
                b.lib.hako_pdu_action_client_poll_alloc(
                    self._handle, info, out_buffer, out_size, out_error
                )
            )
        )
        pointer = out_buffer[0]
        try:
            b.check(out_error[0])
            pdu = b"" if pointer == b.ffi.NULL else bytes(
                b.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            b.lib.hako_pdu_action_buffer_free(pointer)
        goal = None if event == ActionClientEvent.NONE else ClientGoalHandle(
            b.goal_bytes(info.goal)
        )
        return ActionClientPollResult(
            event=event,
            action_name=b.ffi.string(info.action_name).decode("utf-8"),
            goal=goal,
            decision=ActionDecision(int(info.decision)),
            terminal_status=ActionTerminalStatus(int(info.terminal_status)),
            feedback_sequence=int(info.feedback_sequence),
            pdu=pdu,
        )

    def stop(self) -> None:
        if not self._closed:
            self._binding.check(
                self._binding.lib.hako_pdu_action_client_stop(self._handle)
            )

    def close(self) -> None:
        if not self._closed:
            self._binding.lib.hako_pdu_action_client_destroy(self._handle)
            self._handle = self._binding.ffi.NULL
            self._closed = True

    def __enter__(self) -> "ActionClient":
        return self

    def __exit__(self, *_args) -> None:
        self.close()


class ActionServer:
    def __init__(
        self,
        library_path: str | Path,
        node_id: str,
        action_config_path: str | Path,
        endpoint_config_path: str | Path,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
    ):
        self._binding = _ActionBinding(library_path)
        b = self._binding
        self._handle = b.lib.hako_pdu_action_server_create(
            b.encode(node_id),
            b.encode(action_config_path),
            b.encode(endpoint_config_path),
            delta_time_usec,
            b.encode(time_source_type),
        )
        if self._handle == b.ffi.NULL:
            raise ActionError(ActionErrorCode.INITIALIZE)
        self._closed = False

    def start(self) -> None:
        self._binding.check(
            self._binding.lib.hako_pdu_action_server_start(self._handle)
        )

    def is_running(self) -> bool:
        b = self._binding
        out_running = b.ffi.new("int *")
        b.check(b.lib.hako_pdu_action_server_is_running(self._handle, out_running))
        return bool(out_running[0])

    def poll(self) -> ActionServerPollResult:
        b = self._binding
        info = b.ffi.new("hako_pdu_action_server_event_info_t *")
        out_buffer = b.ffi.new("uint8_t **")
        out_size = b.ffi.new("size_t *")
        out_error = b.ffi.new("hako_pdu_action_error_t *")
        event = ActionServerEvent(
            int(
                b.lib.hako_pdu_action_server_poll_alloc(
                    self._handle, info, out_buffer, out_size, out_error
                )
            )
        )
        pointer = out_buffer[0]
        try:
            b.check(out_error[0])
            pdu = b"" if pointer == b.ffi.NULL else bytes(
                b.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            b.lib.hako_pdu_action_buffer_free(pointer)
        goal = None if event == ActionServerEvent.NONE else ServerGoalHandle(
            b.goal_bytes(info.goal)
        )
        return ActionServerPollResult(
            event=event,
            action_name=b.ffi.string(info.action_name).decode("utf-8"),
            goal=goal,
            runtime_cancel_cause=RuntimeCancelCause(
                int(info.runtime_cancel_cause)
            ),
            pdu=pdu,
        )

    def accept_goal(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call("hako_pdu_action_server_accept_goal", action_name, goal)

    def reject_goal(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call("hako_pdu_action_server_reject_goal", action_name, goal)

    def accept_cancel(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call("hako_pdu_action_server_accept_cancel", action_name, goal)

    def reject_cancel(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call("hako_pdu_action_server_reject_cancel", action_name, goal)

    def create_feedback_buffer(self, action_name: str) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_action_server_create_feedback_buffer_alloc,
            self._handle,
            b.encode(action_name),
        )

    def create_result_buffer(self, action_name: str) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_action_server_create_result_buffer_alloc,
            self._handle,
            b.encode(action_name),
        )

    def send_feedback(
        self, action_name: str, goal: ServerGoalHandle, pdu: bytes
    ) -> None:
        b = self._binding
        native_goal = b.server_goal(goal)
        native_pdu = _borrow_bytes(b, pdu)
        b.check(
            b.lib.hako_pdu_action_server_send_feedback(
                self._handle,
                b.encode(action_name),
                native_goal,
                native_pdu,
                len(pdu),
            )
        )

    def complete(
        self,
        action_name: str,
        goal: ServerGoalHandle,
        status: ActionTerminalStatus,
        pdu: bytes,
    ) -> None:
        b = self._binding
        native_goal = b.server_goal(goal)
        native_pdu = _borrow_bytes(b, pdu)
        b.check(
            b.lib.hako_pdu_action_server_complete(
                self._handle,
                b.encode(action_name),
                native_goal,
                int(status),
                native_pdu,
                len(pdu),
            )
        )

    def stop(self) -> None:
        if not self._closed:
            self._binding.check(
                self._binding.lib.hako_pdu_action_server_stop(self._handle)
            )

    def close(self) -> None:
        if not self._closed:
            self._binding.lib.hako_pdu_action_server_destroy(self._handle)
            self._handle = self._binding.ffi.NULL
            self._closed = True

    def __enter__(self) -> "ActionServer":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    def _goal_call(
        self, function_name: str, action_name: str, goal: ServerGoalHandle
    ) -> None:
        b = self._binding
        function = getattr(b.lib, function_name)
        native_goal = b.server_goal(goal)
        b.check(function(self._handle, b.encode(action_name), native_goal))


class ActionMuxServer:
    """Thin adapter for the native TCP Mux Action Server.

    Goal identity remains ``action_name + ServerGoalHandle``. Transport
    connection identity and routing are intentionally not exposed here.
    """

    def __init__(
        self,
        library_path: str | Path,
        node_id: str,
        action_config_path: str | Path,
        endpoint_mux_config_path: str | Path,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
    ):
        self._binding = _ActionBinding(library_path)
        b = self._binding
        self._handle = b.lib.hako_pdu_action_mux_server_create(
            b.encode(node_id),
            b.encode(action_config_path),
            b.encode(endpoint_mux_config_path),
            delta_time_usec,
            b.encode(time_source_type),
        )
        if self._handle == b.ffi.NULL:
            raise ActionError(ActionErrorCode.INITIALIZE)
        self._closed = False

    def start(self) -> None:
        self._binding.check(
            self._binding.lib.hako_pdu_action_mux_server_start(self._handle)
        )

    def poll(self) -> ActionServerPollResult:
        b = self._binding
        info = b.ffi.new("hako_pdu_action_server_event_info_t *")
        out_buffer = b.ffi.new("uint8_t **")
        out_size = b.ffi.new("size_t *")
        out_error = b.ffi.new("hako_pdu_action_error_t *")
        event = ActionServerEvent(
            int(
                b.lib.hako_pdu_action_mux_server_poll_alloc(
                    self._handle, info, out_buffer, out_size, out_error
                )
            )
        )
        pointer = out_buffer[0]
        try:
            b.check(out_error[0])
            pdu = b"" if pointer == b.ffi.NULL else bytes(
                b.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            b.lib.hako_pdu_action_buffer_free(pointer)
        goal = None if event == ActionServerEvent.NONE else ServerGoalHandle(
            b.goal_bytes(info.goal)
        )
        return ActionServerPollResult(
            event=event,
            action_name=b.ffi.string(info.action_name).decode("utf-8"),
            goal=goal,
            runtime_cancel_cause=RuntimeCancelCause(
                int(info.runtime_cancel_cause)
            ),
            pdu=pdu,
        )

    def accept_goal(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call(
            "hako_pdu_action_mux_server_accept_goal", action_name, goal
        )

    def reject_goal(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call(
            "hako_pdu_action_mux_server_reject_goal", action_name, goal
        )

    def accept_cancel(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call(
            "hako_pdu_action_mux_server_accept_cancel", action_name, goal
        )

    def reject_cancel(self, action_name: str, goal: ServerGoalHandle) -> None:
        self._goal_call(
            "hako_pdu_action_mux_server_reject_cancel", action_name, goal
        )

    def create_feedback_buffer(self, action_name: str) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_action_mux_server_create_feedback_buffer_alloc,
            self._handle,
            b.encode(action_name),
        )

    def create_result_buffer(self, action_name: str) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_action_mux_server_create_result_buffer_alloc,
            self._handle,
            b.encode(action_name),
        )

    def send_feedback(
        self, action_name: str, goal: ServerGoalHandle, pdu: bytes
    ) -> None:
        b = self._binding
        native_goal = b.server_goal(goal)
        native_pdu = _borrow_bytes(b, pdu)
        b.check(
            b.lib.hako_pdu_action_mux_server_send_feedback(
                self._handle,
                b.encode(action_name),
                native_goal,
                native_pdu,
                len(pdu),
            )
        )

    def complete(
        self,
        action_name: str,
        goal: ServerGoalHandle,
        status: ActionTerminalStatus,
        pdu: bytes,
    ) -> None:
        b = self._binding
        native_goal = b.server_goal(goal)
        native_pdu = _borrow_bytes(b, pdu)
        b.check(
            b.lib.hako_pdu_action_mux_server_complete(
                self._handle,
                b.encode(action_name),
                native_goal,
                int(status),
                native_pdu,
                len(pdu),
            )
        )

    def connected_count(self) -> int:
        return int(
            self._binding.lib.hako_pdu_action_mux_server_connected_count(
                self._handle
            )
        )

    def expected_count(self) -> int:
        return int(
            self._binding.lib.hako_pdu_action_mux_server_expected_count(
                self._handle
            )
        )

    def is_ready(self) -> bool:
        return bool(
            self._binding.lib.hako_pdu_action_mux_server_is_ready(self._handle)
        )

    def stop(self) -> None:
        if not self._closed:
            self._binding.check(
                self._binding.lib.hako_pdu_action_mux_server_stop(self._handle)
            )

    def close(self) -> None:
        if not self._closed:
            self._binding.lib.hako_pdu_action_mux_server_destroy(self._handle)
            self._handle = self._binding.ffi.NULL
            self._closed = True

    def __enter__(self) -> "ActionMuxServer":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    def _goal_call(
        self, function_name: str, action_name: str, goal: ServerGoalHandle
    ) -> None:
        b = self._binding
        function = getattr(b.lib, function_name)
        native_goal = b.server_goal(goal)
        b.check(function(self._handle, b.encode(action_name), native_goal))
