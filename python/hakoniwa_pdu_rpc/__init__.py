"""Hakoniwa PDU RPC Python bindings and validation utilities."""

from __future__ import annotations

import os
import sys
from pathlib import Path


_DLL_DIRECTORY_HANDLES = []


def _add_windows_runtime_search_dirs() -> None:
    """Register native dependency directories before importing CFFI bindings."""
    if sys.platform != "win32" or not hasattr(os, "add_dll_directory"):
        return

    candidates = []
    for variable in ("HAKO_PDU_RPC_LIBRARY", "HAKO_PDU_ENDPOINT_LIBRARY"):
        value = os.environ.get(variable)
        if value:
            candidates.append(Path(value).expanduser().resolve().parent)

    seen = set()
    for candidate in candidates:
        if candidate in seen or not candidate.is_dir():
            continue
        seen.add(candidate)
        try:
            # Keep the returned handles alive for the lifetime of the package.
            _DLL_DIRECTORY_HANDLES.append(os.add_dll_directory(str(candidate)))
        except OSError:
            continue


_add_windows_runtime_search_dirs()

from .auto_wire import ServiceWire, TypedRpcClient, load_service_wire, make_typed_client
from .cffi_api import (
    ClientEvent,
    ClientPollResult,
    RpcClient as CffiRpcClient,
    RpcError,
    RpcServer,
    ServerEvent,
    ServerPollResult,
)
from .client import RpcCanceledError, RpcClient, RpcTimeoutError
from .future import RpcFuture
from .mux_server import RpcMuxServer

__all__ = [
    "CffiRpcClient",
    "ClientEvent",
    "ClientPollResult",
    "RpcCanceledError",
    "RpcClient",
    "RpcError",
    "RpcFuture",
    "RpcMuxServer",
    "RpcServer",
    "RpcTimeoutError",
    "ServerEvent",
    "ServerPollResult",
    "ServiceWire",
    "TypedRpcClient",
    "load_service_wire",
    "make_typed_client",
    "validate_configs",
]
