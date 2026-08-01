from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Optional

from cffi import FFI


class RpcError(RuntimeError):
    pass


class ClientEvent(IntEnum):
    NONE = 0
    RESPONSE_IN = 1
    RESPONSE_CANCEL = 2
    RESPONSE_TIMEOUT = 3


class ServerEvent(IntEnum):
    NONE = 0
    REQUEST_IN = 1
    REQUEST_CANCEL = 2


@dataclass(frozen=True)
class ClientPollResult:
    event: ClientEvent
    service_name: str
    pdu: bytes


@dataclass(frozen=True)
class ServerPollResult:
    event: ServerEvent
    request_token: int
    service_name: str
    client_name: str
    pdu: bytes


_CDEF = r"""
typedef struct hako_pdu_rpc_client_handle hako_pdu_rpc_client_handle_t;
typedef struct hako_pdu_rpc_server_handle hako_pdu_rpc_server_handle_t;

typedef int hako_pdu_rpc_error_t;
typedef int hako_pdu_rpc_client_event_t;
typedef int hako_pdu_rpc_server_event_t;

typedef struct {
    char service_name[128];
    size_t pdu_size;
} hako_pdu_rpc_response_info_t;

typedef struct {
    uint64_t request_token;
    char service_name[128];
    char client_name[128];
    size_t pdu_size;
} hako_pdu_rpc_request_info_t;

void hako_pdu_rpc_buffer_free(uint8_t* buffer);

hako_pdu_rpc_client_handle_t* hako_pdu_rpc_client_create(
    const char*, const char*, const char*, const char*, uint64_t, const char*);
void hako_pdu_rpc_client_destroy(hako_pdu_rpc_client_handle_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_client_start(hako_pdu_rpc_client_handle_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_client_stop(hako_pdu_rpc_client_handle_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_client_create_request_buffer_alloc(
    hako_pdu_rpc_client_handle_t*, const char*, uint8_t**, size_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_client_call(
    hako_pdu_rpc_client_handle_t*, const char*, const uint8_t*, size_t, uint64_t);
hako_pdu_rpc_error_t hako_pdu_rpc_client_cancel(
    hako_pdu_rpc_client_handle_t*, const char*);
hako_pdu_rpc_client_event_t hako_pdu_rpc_client_poll_alloc(
    hako_pdu_rpc_client_handle_t*, hako_pdu_rpc_response_info_t*,
    uint8_t**, size_t*, hako_pdu_rpc_error_t*);

hako_pdu_rpc_server_handle_t* hako_pdu_rpc_server_create(
    const char*, const char*, const char*, uint64_t, const char*);
void hako_pdu_rpc_server_destroy(hako_pdu_rpc_server_handle_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_server_start(hako_pdu_rpc_server_handle_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_server_stop(hako_pdu_rpc_server_handle_t*);
hako_pdu_rpc_server_event_t hako_pdu_rpc_server_poll_alloc(
    hako_pdu_rpc_server_handle_t*, hako_pdu_rpc_request_info_t*,
    uint8_t**, size_t*, hako_pdu_rpc_error_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_server_create_reply_buffer_alloc(
    hako_pdu_rpc_server_handle_t*, uint64_t, uint8_t, int32_t,
    uint8_t**, size_t*);
hako_pdu_rpc_error_t hako_pdu_rpc_server_send_reply(
    hako_pdu_rpc_server_handle_t*, uint64_t, const uint8_t*, size_t);
hako_pdu_rpc_error_t hako_pdu_rpc_server_send_cancel_reply(
    hako_pdu_rpc_server_handle_t*, uint64_t, const uint8_t*, size_t);
"""


class _Binding:
    def __init__(self, library_path: str | Path):
        self.ffi = FFI()
        self.ffi.cdef(_CDEF)
        self.lib = self.ffi.dlopen(str(library_path))

    def encode(self, value: str | Path):
        return self.ffi.new("char[]", str(value).encode("utf-8"))

    def take_bytes(self, pointer, size: int) -> bytes:
        if pointer == self.ffi.NULL or size == 0:
            return b""
        try:
            return bytes(self.ffi.buffer(pointer, size))
        finally:
            self.lib.hako_pdu_rpc_buffer_free(pointer)

    def allocated_call(self, function, *args) -> bytes:
        out_buffer = self.ffi.new("uint8_t **")
        out_size = self.ffi.new("size_t *")
        error = int(function(*args, out_buffer, out_size))
        pointer = out_buffer[0]
        try:
            if error != 0:
                raise RpcError(f"native RPC error: {error}")
            return b"" if pointer == self.ffi.NULL else bytes(
                self.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            self.lib.hako_pdu_rpc_buffer_free(pointer)


def _borrow_bytes(binding: _Binding, data: bytes):
    if not data:
        return binding.ffi.NULL
    return binding.ffi.new("uint8_t[]", data)


class RpcClient:
    def __init__(
        self,
        library_path: str | Path,
        node_id: str,
        client_name: str,
        service_config_path: str | Path,
        endpoint_config_path: str | Path,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
    ):
        self._binding = _Binding(library_path)
        b = self._binding
        self._handle = b.lib.hako_pdu_rpc_client_create(
            b.encode(node_id),
            b.encode(client_name),
            b.encode(service_config_path),
            b.encode(endpoint_config_path),
            delta_time_usec,
            b.encode(time_source_type),
        )
        if self._handle == b.ffi.NULL:
            raise RpcError("failed to create RPC client")
        self._closed = False

    def start(self) -> None:
        self._check(self._binding.lib.hako_pdu_rpc_client_start(self._handle))

    def create_request_buffer(self, service_name: str) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_rpc_client_create_request_buffer_alloc,
            self._handle,
            b.encode(service_name),
        )

    def call(self, service_name: str, pdu: bytes, timeout_usec: int) -> None:
        b = self._binding
        native = _borrow_bytes(b, pdu)
        self._check(
            b.lib.hako_pdu_rpc_client_call(
                self._handle,
                b.encode(service_name),
                native,
                len(pdu),
                timeout_usec,
            )
        )

    def cancel(self, service_name: str) -> None:
        b = self._binding
        self._check(
            b.lib.hako_pdu_rpc_client_cancel(self._handle, b.encode(service_name))
        )

    def poll(self) -> ClientPollResult:
        b = self._binding
        info = b.ffi.new("hako_pdu_rpc_response_info_t *")
        out_buffer = b.ffi.new("uint8_t **")
        out_size = b.ffi.new("size_t *")
        out_error = b.ffi.new("hako_pdu_rpc_error_t *")
        event = ClientEvent(
            int(
                b.lib.hako_pdu_rpc_client_poll_alloc(
                    self._handle, info, out_buffer, out_size, out_error
                )
            )
        )
        pointer = out_buffer[0]
        try:
            if int(out_error[0]) != 0:
                raise RpcError(f"native RPC error: {int(out_error[0])}")
            data = b"" if pointer == b.ffi.NULL else bytes(
                b.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            b.lib.hako_pdu_rpc_buffer_free(pointer)
        return ClientPollResult(
            event=event,
            service_name=b.ffi.string(info.service_name).decode("utf-8"),
            pdu=data,
        )

    def close(self) -> None:
        if not self._closed:
            self._binding.lib.hako_pdu_rpc_client_destroy(self._handle)
            self._handle = self._binding.ffi.NULL
            self._closed = True

    def __enter__(self) -> "RpcClient":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    @staticmethod
    def _check(error: int) -> None:
        if int(error) != 0:
            raise RpcError(f"native RPC error: {int(error)}")


class RpcServer:
    def __init__(
        self,
        library_path: str | Path,
        node_id: str,
        service_config_path: str | Path,
        endpoint_config_path: str | Path,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
    ):
        self._binding = _Binding(library_path)
        b = self._binding
        self._handle = b.lib.hako_pdu_rpc_server_create(
            b.encode(node_id),
            b.encode(service_config_path),
            b.encode(endpoint_config_path),
            delta_time_usec,
            b.encode(time_source_type),
        )
        if self._handle == b.ffi.NULL:
            raise RpcError("failed to create RPC server")
        self._closed = False

    def start(self) -> None:
        self._check(self._binding.lib.hako_pdu_rpc_server_start(self._handle))

    def poll(self) -> ServerPollResult:
        b = self._binding
        info = b.ffi.new("hako_pdu_rpc_request_info_t *")
        out_buffer = b.ffi.new("uint8_t **")
        out_size = b.ffi.new("size_t *")
        out_error = b.ffi.new("hako_pdu_rpc_error_t *")
        event = ServerEvent(
            int(
                b.lib.hako_pdu_rpc_server_poll_alloc(
                    self._handle, info, out_buffer, out_size, out_error
                )
            )
        )
        pointer = out_buffer[0]
        try:
            if int(out_error[0]) != 0:
                raise RpcError(f"native RPC error: {int(out_error[0])}")
            data = b"" if pointer == b.ffi.NULL else bytes(
                b.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            b.lib.hako_pdu_rpc_buffer_free(pointer)
        return ServerPollResult(
            event=event,
            request_token=int(info.request_token),
            service_name=b.ffi.string(info.service_name).decode("utf-8"),
            client_name=b.ffi.string(info.client_name).decode("utf-8"),
            pdu=data,
        )

    def create_reply_buffer(
        self, request_token: int, status: int, result_code: int
    ) -> bytes:
        b = self._binding
        return b.allocated_call(
            b.lib.hako_pdu_rpc_server_create_reply_buffer_alloc,
            self._handle,
            request_token,
            status,
            result_code,
        )

    def send_reply(self, request_token: int, pdu: bytes) -> None:
        b = self._binding
        native = _borrow_bytes(b, pdu)
        self._check(
            b.lib.hako_pdu_rpc_server_send_reply(
                self._handle, request_token, native, len(pdu)
            )
        )

    def send_cancel_reply(self, request_token: int, pdu: bytes) -> None:
        b = self._binding
        native = _borrow_bytes(b, pdu)
        self._check(
            b.lib.hako_pdu_rpc_server_send_cancel_reply(
                self._handle, request_token, native, len(pdu)
            )
        )

    def close(self) -> None:
        if not self._closed:
            self._binding.lib.hako_pdu_rpc_server_destroy(self._handle)
            self._handle = self._binding.ffi.NULL
            self._closed = True

    def __enter__(self) -> "RpcServer":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    @staticmethod
    def _check(error: int) -> None:
        if int(error) != 0:
            raise RpcError(f"native RPC error: {int(error)}")
