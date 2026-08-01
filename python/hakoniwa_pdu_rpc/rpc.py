from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Optional

from ._c_rpc_ffi import ffi, lib

_DEFAULT_BUFFER_SIZE = 4 * 1024 * 1024


class RpcError(RuntimeError):
    pass


class ClientEvent(IntEnum):
    NONE = lib.HAKO_PDU_RPC_CLIENT_EVENT_NONE
    RESPONSE_IN = lib.HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN
    RESPONSE_CANCEL = lib.HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL
    RESPONSE_TIMEOUT = lib.HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT


class ServerEvent(IntEnum):
    NONE = lib.HAKO_PDU_RPC_SERVER_EVENT_NONE
    REQUEST_IN = lib.HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN
    REQUEST_CANCEL = lib.HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL


@dataclass(frozen=True)
class ClientPollResult:
    event: ClientEvent
    service_name: str = ""
    pdu: bytes = b""


@dataclass(frozen=True)
class ServerPollResult:
    event: ServerEvent
    request_token: int = 0
    service_name: str = ""
    client_name: str = ""
    pdu: bytes = b""


def _cstr(value: str):
    return ffi.new("char[]", value.encode("utf-8"))


def _decode_name(value) -> str:
    return ffi.string(value).decode("utf-8")


def _check(error: int, operation: str) -> None:
    if error != lib.HAKO_PDU_RPC_OK:
        raise RpcError(f"{operation} failed: error={error}")


class RpcClient:
    def __init__(
        self,
        node_id: str,
        client_name: str,
        service_config_path: str,
        endpoint_config_path: str,
        *,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
        buffer_size: int = _DEFAULT_BUFFER_SIZE,
    ) -> None:
        self._buffer_size = buffer_size
        self._handle = lib.hako_pdu_rpc_client_create(
            _cstr(node_id),
            _cstr(client_name),
            _cstr(service_config_path),
            _cstr(endpoint_config_path),
            delta_time_usec,
            _cstr(time_source_type),
        )
        if self._handle == ffi.NULL:
            raise RpcError("client create failed")

    def start(self) -> None:
        _check(lib.hako_pdu_rpc_client_start(self._handle), "client start")

    def stop(self) -> None:
        if self._handle != ffi.NULL:
            _check(lib.hako_pdu_rpc_client_stop(self._handle), "client stop")

    def close(self) -> None:
        if self._handle != ffi.NULL:
            lib.hako_pdu_rpc_client_destroy(self._handle)
            self._handle = ffi.NULL

    def __enter__(self) -> "RpcClient":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def create_request_buffer(self, service_name: str) -> bytearray:
        size = ffi.new("size_t *")
        error = lib.hako_pdu_rpc_client_create_request_buffer(
            self._handle, _cstr(service_name), ffi.NULL, 0, size
        )
        if error not in (lib.HAKO_PDU_RPC_OK, lib.HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL):
            _check(error, "create request buffer")
        buffer = ffi.new("uint8_t[]", size[0])
        _check(
            lib.hako_pdu_rpc_client_create_request_buffer(
                self._handle, _cstr(service_name), buffer, size[0], size
            ),
            "create request buffer",
        )
        return bytearray(ffi.buffer(buffer, size[0]))

    def call(self, service_name: str, request_pdu: bytes | bytearray, timeout_usec: int) -> None:
        data = bytes(request_pdu)
        buffer = ffi.from_buffer("uint8_t[]", data)
        _check(
            lib.hako_pdu_rpc_client_call(
                self._handle, _cstr(service_name), buffer, len(data), timeout_usec
            ),
            "client call",
        )

    def poll(self) -> ClientPollResult:
        info = ffi.new("hako_pdu_rpc_response_info_t *")
        size = ffi.new("size_t *")
        error = ffi.new("hako_pdu_rpc_error_t *")
        buffer = ffi.new("uint8_t[]", self._buffer_size)
        event = lib.hako_pdu_rpc_client_poll(
            self._handle, info, buffer, self._buffer_size, size, error
        )
        _check(error[0], "client poll")
        return ClientPollResult(
            ClientEvent(event),
            _decode_name(info.service_name) if event else "",
            bytes(ffi.buffer(buffer, size[0])) if event else b"",
        )

    def cancel(self, service_name: str) -> None:
        _check(lib.hako_pdu_rpc_client_cancel(self._handle, _cstr(service_name)), "client cancel")


class RpcServer:
    def __init__(
        self,
        node_id: str,
        service_config_path: str,
        endpoint_config_path: str,
        *,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
        buffer_size: int = _DEFAULT_BUFFER_SIZE,
    ) -> None:
        self._buffer_size = buffer_size
        self._handle = lib.hako_pdu_rpc_server_create(
            _cstr(node_id),
            _cstr(service_config_path),
            _cstr(endpoint_config_path),
            delta_time_usec,
            _cstr(time_source_type),
        )
        if self._handle == ffi.NULL:
            raise RpcError("server create failed")

    def start(self) -> None:
        _check(lib.hako_pdu_rpc_server_start(self._handle), "server start")

    def stop(self) -> None:
        if self._handle != ffi.NULL:
            _check(lib.hako_pdu_rpc_server_stop(self._handle), "server stop")

    def close(self) -> None:
        if self._handle != ffi.NULL:
            lib.hako_pdu_rpc_server_destroy(self._handle)
            self._handle = ffi.NULL

    def __enter__(self) -> "RpcServer":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def poll(self) -> ServerPollResult:
        info = ffi.new("hako_pdu_rpc_request_info_t *")
        size = ffi.new("size_t *")
        error = ffi.new("hako_pdu_rpc_error_t *")
        buffer = ffi.new("uint8_t[]", self._buffer_size)
        event = lib.hako_pdu_rpc_server_poll(
            self._handle, info, buffer, self._buffer_size, size, error
        )
        _check(error[0], "server poll")
        return ServerPollResult(
            ServerEvent(event),
            int(info.request_token) if event else 0,
            _decode_name(info.service_name) if event else "",
            _decode_name(info.client_name) if event else "",
            bytes(ffi.buffer(buffer, size[0])) if event else b"",
        )

    def create_reply_buffer(self, request_token: int, status: int, result_code: int) -> bytearray:
        size = ffi.new("size_t *")
        error = lib.hako_pdu_rpc_server_create_reply_buffer(
            self._handle, request_token, status, result_code, ffi.NULL, 0, size
        )
        if error not in (lib.HAKO_PDU_RPC_OK, lib.HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL):
            _check(error, "create reply buffer")
        buffer = ffi.new("uint8_t[]", size[0])
        _check(
            lib.hako_pdu_rpc_server_create_reply_buffer(
                self._handle, request_token, status, result_code, buffer, size[0], size
            ),
            "create reply buffer",
        )
        return bytearray(ffi.buffer(buffer, size[0]))

    def send_reply(self, request_token: int, response_pdu: bytes | bytearray) -> None:
        data = bytes(response_pdu)
        buffer = ffi.from_buffer("uint8_t[]", data)
        _check(
            lib.hako_pdu_rpc_server_send_reply(self._handle, request_token, buffer, len(data)),
            "send reply",
        )

    def send_cancel_reply(self, request_token: int, response_pdu: bytes | bytearray) -> None:
        data = bytes(response_pdu)
        buffer = ffi.from_buffer("uint8_t[]", data)
        _check(
            lib.hako_pdu_rpc_server_send_cancel_reply(
                self._handle, request_token, buffer, len(data)
            ),
            "send cancel reply",
        )
