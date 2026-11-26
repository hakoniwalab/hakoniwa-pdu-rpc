import os
from pathlib import Path
import time

import pytest

from hakoniwa_pdu_rpc import RpcClient, make_typed_client


@pytest.mark.skipif(
    "HAKO_PDU_RPC_SHM_TEST_DIR" not in os.environ,
    reason="requires the polling core-pro fixture in test/shm_core_pro_integration.bash",
)
def test_shm_round_trip_without_endpoint_config():
    work = Path(os.environ["HAKO_PDU_RPC_SHM_TEST_DIR"])
    with RpcClient(
        os.environ["HAKO_PDU_RPC_LIBRARY"],
        "shm_client_node",
        "ShmClient",
        work / "service.json",
    ) as rpc:
        rpc.start()
        print("CLIENT registered", flush=True)
        deadline = time.monotonic() + 10
        while not (work / "start-go").is_file():
            assert time.monotonic() < deadline, "core-pro start timed out"
            time.sleep(0.001)

        service = make_typed_client(rpc, "Service/Add", "hako_srv_msgs/AddTwoInts")
        request = service.create_request()
        request.a, request.b = 19, 23
        assert service.call_async(request, timeout_usec=0).result(timeout=5).sum == 42
    print("PASS shm round trip sum=42", flush=True)
