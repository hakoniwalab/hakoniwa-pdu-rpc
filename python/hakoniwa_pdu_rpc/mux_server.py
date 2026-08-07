from __future__ import annotations

from pathlib import Path

from .cffi_api import (
    RpcError,
    ServerEvent,
    ServerPollResult,
    _borrow_bytes,
    _get_binding,
)


class RpcMuxServer:
    """Thin Python wrapper for the native multiplexed RPC server."""

    def __init__(
        self,
        library_path: str | Path,
        node_id: str,
        service_config_path: str | Path,
        endpoint_mux_config_path: str | Path,
        delta_time_usec: int = 1000,
        time_source_type: str = "real",
    ):
        self._binding = _get_binding(library_path)
        binding = self._binding
        self._handle = binding.lib.hako_pdu_rpc_mux_server_create(
            binding.encode(node_id),
            binding.encode(service_config_path),
            binding.encode(endpoint_mux_config_path),
            delta_time_usec,
            binding.encode(time_source_type),
        )
        if self._handle == binding.ffi.NULL:
            raise RpcError("failed to create RPC mux server")
        self._closed = False

    def start(self) -> None:
        self._check(
            self._binding.lib.hako_pdu_rpc_mux_server_start(self._handle)
        )

    def stop(self) -> None:
        self._check(
            self._binding.lib.hako_pdu_rpc_mux_server_stop(self._handle)
        )

    def poll(self) -> ServerPollResult:
        binding = self._binding
        info = binding.ffi.new("hako_pdu_rpc_request_info_t *")
        out_buffer = binding.ffi.new("uint8_t **")
        out_size = binding.ffi.new("size_t *")
        out_error = binding.ffi.new("hako_pdu_rpc_error_t *")
        event = ServerEvent(
            int(
                binding.lib.hako_pdu_rpc_mux_server_poll_alloc(
                    self._handle,
                    info,
                    out_buffer,
                    out_size,
                    out_error,
                )
            )
        )
        pointer = out_buffer[0]
        try:
            error = int(out_error[0])
            if error != 0:
                raise RpcError(f"native RPC error: {error}")
            data = b"" if pointer == binding.ffi.NULL else bytes(
                binding.ffi.buffer(pointer, int(out_size[0]))
            )
        finally:
            binding.lib.hako_pdu_rpc_buffer_free(pointer)
        return ServerPollResult(
            event=event,
            request_token=int(info.request_token),
            service_name=binding.ffi.string(info.service_name).decode("utf-8"),
            client_name=binding.ffi.string(info.client_name).decode("utf-8"),
            pdu=data,
        )

    def create_reply_buffer(
        self,
        request_token: int,
        status: int,
        result_code: int,
    ) -> bytes:
        binding = self._binding
        return binding.allocated_call(
            binding.lib.hako_pdu_rpc_mux_server_create_reply_buffer_alloc,
            self._handle,
            request_token,
            status,
            result_code,
        )

    def send_reply(self, request_token: int, pdu: bytes) -> None:
        binding = self._binding
        native = _borrow_bytes(binding, pdu)
        self._check(
            binding.lib.hako_pdu_rpc_mux_server_send_reply(
                self._handle,
                request_token,
                native,
                len(pdu),
            )
        )

    def send_cancel_reply(self, request_token: int, pdu: bytes) -> None:
        binding = self._binding
        native = _borrow_bytes(binding, pdu)
        self._check(
            binding.lib.hako_pdu_rpc_mux_server_send_cancel_reply(
                self._handle,
                request_token,
                native,
                len(pdu),
            )
        )

    def connected_count(self) -> int:
        return int(
            self._binding.lib.hako_pdu_rpc_mux_server_connected_count(
                self._handle
            )
        )

    def expected_count(self) -> int:
        return int(
            self._binding.lib.hako_pdu_rpc_mux_server_expected_count(
                self._handle
            )
        )

    def is_ready(self) -> bool:
        return bool(
            self._binding.lib.hako_pdu_rpc_mux_server_is_ready(self._handle)
        )

    def close(self) -> None:
        if not self._closed:
            self._binding.lib.hako_pdu_rpc_mux_server_destroy(self._handle)
            self._handle = self._binding.ffi.NULL
            self._closed = True

    def __enter__(self) -> "RpcMuxServer":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    @staticmethod
    def _check(error: int) -> None:
        if int(error) != 0:
            raise RpcError(f"native RPC error: {int(error)}")
