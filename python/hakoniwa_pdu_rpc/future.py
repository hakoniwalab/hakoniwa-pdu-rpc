from __future__ import annotations

from concurrent.futures import Future
from typing import Callable, Generic, TypeVar


T = TypeVar("T")


class RpcFuture(Generic[T]):
    """ROS-independent handle for an in-flight RPC call.

    Completion is driven by the RPC client's background worker. ``cancel()``
    requests protocol-level cancellation; terminal completion is still reported
    through ``result()`` or ``exception()``.
    """

    def __init__(self, cancel_callback: Callable[[], None]):
        self._future: Future[T] = Future()
        self._cancel_callback = cancel_callback

    def cancel(self) -> bool:
        if self._future.done():
            return False
        self._cancel_callback()
        return True

    def cancelled(self) -> bool:
        return self._future.cancelled()

    def running(self) -> bool:
        return self._future.running()

    def done(self) -> bool:
        return self._future.done()

    def result(self, timeout: float | None = None) -> T:
        return self._future.result(timeout=timeout)

    def exception(self, timeout: float | None = None) -> BaseException | None:
        return self._future.exception(timeout=timeout)

    def add_done_callback(self, fn: Callable[["RpcFuture[T]"], object]) -> None:
        self._future.add_done_callback(lambda _future: fn(self))

    def _set_running_or_notify_cancel(self) -> bool:
        return self._future.set_running_or_notify_cancel()

    def _set_result(self, result: T) -> None:
        self._future.set_result(result)

    def _set_exception(self, exception: BaseException) -> None:
        self._future.set_exception(exception)
