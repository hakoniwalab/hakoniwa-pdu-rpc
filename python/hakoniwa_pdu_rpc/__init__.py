"""Hakoniwa PDU RPC validation and Python binding utilities."""

from pkgutil import extend_path

# The pure-Python package lives under python/, while the compiled CFFI module
# is generated under build/python/. Extend the package search path so both
# locations form one package when both roots are present on PYTHONPATH.
__path__ = extend_path(__path__, __name__)

from .rpc import (
    ClientEvent,
    ClientPollResult,
    RpcClient,
    RpcError,
    RpcServer,
    ServerEvent,
    ServerPollResult,
)

__all__ = [
    "ClientEvent",
    "ClientPollResult",
    "RpcClient",
    "RpcError",
    "RpcServer",
    "ServerEvent",
    "ServerPollResult",
    "validate_configs",
]
