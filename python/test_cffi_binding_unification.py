from __future__ import annotations

import os
from pathlib import Path

from hakoniwa_pdu_rpc.action_cffi import ActionMuxServer, _ActionBinding
from hakoniwa_pdu_rpc.cffi_api import _get_binding


def test_service_mux_and_action_share_one_ffi_and_library_binding():
    library = os.environ["HAKO_PDU_RPC_LIBRARY"]
    first = _get_binding(library)
    second = _get_binding(Path(library).resolve())
    action = _ActionBinding(library)

    assert first is second
    assert action.ffi is first.ffi
    assert action.lib is first.lib
    assert ActionMuxServer.__module__ == "hakoniwa_pdu_rpc.action_cffi"
