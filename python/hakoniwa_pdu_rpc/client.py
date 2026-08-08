from __future__ import annotations

import threading
import time
from pathlib import Path

from .cffi_api import ClientEvent, RpcClient as CffiRpcClient, RpcError
from .future import RpcFuture


class RpcTimeoutError(RpcError):
    """The RPC timed out and reached terminal cancellation."""


class RpcCanceledError(RpcError):
    """The RPC completed with a cancel response."""


class RpcClient:
    """High-level RPC client with synchronous and non-blocking call APIs.

    Native PDU buffers never escape the CFFI layer. Responses returned by
    :meth:`call` and :meth:`call_async` are ordinary Python ``bytes``.

    The underlying native client exposes one shared response queue, so one
    ``RpcClient`` instance supports one in-flight request at a time. Create
    separate clients when independent concurrent calls are required.
    """

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
        self._client = CffiRpcClient(
            library_path,
            node_id,
            client_name,
            service_config_path,
            endpoint_config_path,
            delta_time_usec,
            time_source_type,
        )
        self._inflight_lock = threading.Lock()

    def start(self) -> None:
        self._client.start()

    def create_request_buffer(self, service_name: str) -> bytes:
        return self._client.create_request_buffer(service_name)

    def call(
        self,
        service_name: str,
        pdu: bytes,
        timeout_usec: int,
        *,
        cancel_on_timeout: bool = True,
        connect_timeout_sec: float = 3.0,
        event_timeout_sec: float = 5.0,
        poll_interval_sec: float = 0.001,
    ) -> bytes:
        """Send one request and synchronously wait for terminal completion."""
        return self.call_async(
            service_name,
            pdu,
            timeout_usec,
            cancel_on_timeout=cancel_on_timeout,
            connect_timeout_sec=connect_timeout_sec,
            event_timeout_sec=event_timeout_sec,
            poll_interval_sec=poll_interval_sec,
        ).result()

    def call_async(
        self,
        service_name: str,
        pdu: bytes,
        timeout_usec: int,
        *,
        cancel_on_timeout: bool = True,
        connect_timeout_sec: float = 3.0,
        event_timeout_sec: float = 5.0,
        poll_interval_sec: float = 0.001,
    ) -> RpcFuture[bytes]:
        """Start one RPC call and return immediately with an :class:`RpcFuture`.

        The worker owns the existing timeout, cancel, and response/cancel race
        state machine. The returned future is independent of ROS and asyncio;
        adapters can use ``add_done_callback()`` or bridge ``result()`` into
        their own executor model without blocking the caller thread.
        """
        if not self._inflight_lock.acquire(blocking=False):
            raise RpcError("another RPC call is already in flight on this client")

        future = RpcFuture[bytes](lambda: self.cancel(service_name))

        def run() -> None:
            if not future._set_running_or_notify_cancel():
                self._inflight_lock.release()
                return
            try:
                response = self._call_impl(
                    service_name,
                    pdu,
                    timeout_usec,
                    cancel_on_timeout=cancel_on_timeout,
                    connect_timeout_sec=connect_timeout_sec,
                    event_timeout_sec=event_timeout_sec,
                    poll_interval_sec=poll_interval_sec,
                )
            except BaseException as error:
                outcome_error: BaseException | None = error
                response = None
            else:
                outcome_error = None

            # Future completion invokes user callbacks synchronously. Make the
            # client reusable before publishing terminal completion so a pool
            # may safely lease it again from inside a done callback.
            self._inflight_lock.release()
            if outcome_error is not None:
                future._set_exception(outcome_error)
            else:
                future._set_result(response)

        threading.Thread(
            target=run,
            name=f"hakoniwa-rpc-{service_name}",
            daemon=True,
        ).start()
        return future

    def cancel(self, service_name: str) -> None:
        self._client.cancel(service_name)

    def close(self) -> None:
        self._client.close()

    def __enter__(self) -> "RpcClient":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    def _call_impl(
        self,
        service_name: str,
        pdu: bytes,
        timeout_usec: int,
        *,
        cancel_on_timeout: bool,
        connect_timeout_sec: float,
        event_timeout_sec: float,
        poll_interval_sec: float,
    ) -> bytes:
        self._send_when_connected(
            service_name,
            pdu,
            timeout_usec,
            connect_timeout_sec,
            poll_interval_sec,
        )

        deadline = time.monotonic() + event_timeout_sec
        while time.monotonic() < deadline:
            result = self._client.poll()
            if result.event == ClientEvent.NONE:
                time.sleep(poll_interval_sec)
                continue
            if result.service_name != service_name:
                raise RpcError(
                    f"unexpected service response: {result.service_name!r}"
                )
            if result.event == ClientEvent.RESPONSE_IN:
                return result.pdu
            if result.event == ClientEvent.RESPONSE_CANCEL:
                raise RpcCanceledError(f"RPC canceled: {service_name}")
            if result.event == ClientEvent.RESPONSE_TIMEOUT:
                if not cancel_on_timeout:
                    raise RpcTimeoutError(f"RPC timed out: {service_name}")
                self._client.cancel(service_name)
                return self._wait_after_timeout(
                    service_name,
                    deadline,
                    poll_interval_sec,
                )

        raise RpcError(f"RPC terminal event was not received: {service_name}")

    def _send_when_connected(
        self,
        service_name: str,
        pdu: bytes,
        timeout_usec: int,
        connect_timeout_sec: float,
        poll_interval_sec: float,
    ) -> None:
        deadline = time.monotonic() + connect_timeout_sec
        while True:
            try:
                self._client.call(service_name, pdu, timeout_usec)
                return
            except RpcError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(max(poll_interval_sec, 0.01))

    def _wait_after_timeout(
        self,
        service_name: str,
        deadline: float,
        poll_interval_sec: float,
    ) -> bytes:
        while time.monotonic() < deadline:
            result = self._client.poll()
            if result.event == ClientEvent.NONE:
                time.sleep(poll_interval_sec)
                continue
            if result.service_name != service_name:
                raise RpcError(
                    f"unexpected service response: {result.service_name!r}"
                )
            if result.event == ClientEvent.RESPONSE_IN:
                return result.pdu
            if result.event == ClientEvent.RESPONSE_CANCEL:
                raise RpcTimeoutError(f"RPC timed out and was canceled: {service_name}")

        raise RpcError(
            f"RPC cancel completion was not received after timeout: {service_name}"
        )
