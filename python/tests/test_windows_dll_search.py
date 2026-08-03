from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path
from types import SimpleNamespace


def test_windows_runtime_search_dirs_register_library_parents(monkeypatch, tmp_path):
    package_init = Path(__file__).parents[1] / "hakoniwa_pdu_rpc" / "__init__.py"
    rpc_dir = tmp_path / "rpc" / "bin"
    endpoint_dir = tmp_path / "endpoint" / "bin"
    rpc_dir.mkdir(parents=True)
    endpoint_dir.mkdir(parents=True)

    added = []
    handles = []

    def add_dll_directory(path):
        added.append(Path(path))
        handle = SimpleNamespace(path=path)
        handles.append(handle)
        return handle

    monkeypatch.setenv("HAKO_PDU_RPC_LIBRARY", str(rpc_dir / "hakoniwa_pdu_rpc.dll"))
    monkeypatch.setenv(
        "HAKO_PDU_ENDPOINT_LIBRARY", str(endpoint_dir / "hakoniwa_pdu_endpoint.dll")
    )
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(os, "add_dll_directory", add_dll_directory, raising=False)

    source = package_init.read_text(encoding="utf-8")
    setup_only = source.split("_add_windows_runtime_search_dirs()", 1)[0]
    namespace = {}
    exec(setup_only, namespace)
    namespace["_add_windows_runtime_search_dirs"]()

    assert added == [rpc_dir.resolve(), endpoint_dir.resolve()]
    assert namespace["_DLL_DIRECTORY_HANDLES"] == handles
