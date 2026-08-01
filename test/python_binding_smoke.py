from __future__ import annotations

import ctypes
import os
import sys
import time
from pathlib import Path

_NATIVE_LIBRARY = None
_DLL_DIRECTORY = None


def _prepare_imports() -> Path:
    global _NATIVE_LIBRARY, _DLL_DIRECTORY

    repo_root = Path(__file__).resolve().parents[1]
    build_dir = Path(os.environ.get("HAKO_PDU_RPC_BUILD_DIR", repo_root / "build")).resolve()
    build_type = os.environ.get("HAKO_PDU_RPC_BUILD_TYPE", "Release")
    native_dir = build_dir / "src" / build_type if sys.platform == "win32" else build_dir / "src"

    sys.path.insert(0, str(build_dir / "python"))
    sys.path.insert(0, str(repo_root / "python"))

    if sys.platform == "win32":
        _DLL_DIRECTORY = os.add_dll_directory(str(native_dir))
        library = native_dir / "hakoniwa_pdu_rpc.dll"
    elif sys.platform == "darwin":
        library = native_dir / "libhakoniwa_pdu_rpc.dylib"
    else:
        library = native_dir / "libhakoniwa_pdu_rpc.so"

    if not library.is_file():
        raise RuntimeError(f"C facade library was not found: {library}")
    _NATIVE_LIBRARY = ctypes.CDLL(
        str(library), mode=getattr(ctypes, "RTLD_GLOBAL", 0)
    )
    return repo_root


def main() -> None:
    repo_root = _prepare_imports()
    from hakoniwa_pdu_rpc import RpcClient, RpcServer

    service_config = repo_root / "test" / "configs" / "service_config.json"
    endpoint_config = repo_root / "test" / "configs" / "endpoints.json"

    server = RpcServer(
        "server_node",
        str(service_config),
        str(endpoint_config),
    )
    client = RpcClient(
        "client_node",
        "TestClient",
        str(service_config),
        str(endpoint_config),
    )

    try:
        server.start()
        client.start()
        time.sleep(0.05)
    finally:
        # Preserve the established shutdown order: client before server.
        client.close()
        server.close()


if __name__ == "__main__":
    main()
