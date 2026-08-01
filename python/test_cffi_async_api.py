from __future__ import annotations

import threading
import time
from types import MethodType

import pytest

from hakoniwa_pdu_rpc import RpcClient, RpcError, RpcFuture


def make_client(worker):
    client = RpcClient.__new__(RpcClient)
    client._client = object()
    client._inflight_lock = threading.Lock()
    client._call_impl = MethodType(worker, client)
    client.cancel = MethodType(lambda self, service_name: None, client)
    return client


def test_call_async_returns_immediately_and_completes_future():
    release = threading.Event()

    def worker(self, service_name, pdu, timeout_usec, **kwargs):
        assert service_name == "Service/Add"
        assert pdu == b"request"
        release.wait(timeout=2.0)
        return b"response"

    client = make_client(worker)
    started = time.monotonic()
    future = client.call_async("Service/Add", b"request", 1_000_000)

    assert isinstance(future, RpcFuture)
    assert time.monotonic() - started < 0.2
    assert not future.done()

    release.set()
    assert future.result(timeout=2.0) == b"response"
    assert future.done()


def test_call_async_invokes_done_callback():
    callback_done = threading.Event()
    observed = []

    def worker(self, service_name, pdu, timeout_usec, **kwargs):
        return b"response"

    client = make_client(worker)
    future = client.call_async("Service/Add", b"request", 1_000_000)

    def on_done(completed):
        observed.append(completed.result())
        callback_done.set()

    future.add_done_callback(on_done)
    assert callback_done.wait(timeout=2.0)
    assert observed == [b"response"]


def test_client_rejects_multiple_inflight_calls():
    release = threading.Event()

    def worker(self, service_name, pdu, timeout_usec, **kwargs):
        release.wait(timeout=2.0)
        return b"response"

    client = make_client(worker)
    first = client.call_async("Service/Add", b"one", 1_000_000)

    with pytest.raises(RpcError, match="already in flight"):
        client.call_async("Service/Add", b"two", 1_000_000)

    release.set()
    assert first.result(timeout=2.0) == b"response"
