import json
import sys
import tempfile
import unittest
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1] / "python"
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

from hakoniwa_pdu_rpc import service_config_generator as GENERATOR


def manifest() -> dict:
    return {
        "version": 1,
        "services": [
            {
                "name": "Service/Add",
                "type": "hako_srv_msgs/AddTwoInts",
                "maxClients": 2,
                "clientNamePrefix": "hakoniwa_pdu_ros_add",
                "packetSize": {
                    "requestBaseSize": 296,
                    "responseBaseSize": 288,
                },
                "bufferHeap": {"requestSize": 64, "responseSize": 128},
                "clientEndpoint": {"nodeId": "ros-service"},
                "serverEndpoint": {"nodeId": "server-node"},
            }
        ],
        "transport": {
            "protocol": "tcp",
            "packetVersion": "v2",
            "queueDepth": 16,
            "endpoints": {
                "ros-service": {
                    "role": "client",
                    "remote": {"address": "127.0.0.1", "port": 54010},
                    "options": {"connect_timeout_ms": 2000},
                },
                "server-node": {
                    "role": "server",
                    "local": {"address": "0.0.0.0", "port": 54010},
                },
            },
        },
    }


class GenerateServiceConfigTest(unittest.TestCase):
    def test_generates_native_service_and_tcp_mux_configs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "service.json"
            output = root / "generated"
            source.write_text(json.dumps(manifest()), encoding="utf-8")

            GENERATOR.generate(source, output)

            server = json.loads(
                (output / "rpc-server-services.json").read_text()
            )["services"][0]
            client = json.loads(
                (output / "rpc-client-services.json").read_text()
            )["services"][0]
            self.assertEqual(
                server["server_endpoints"],
                [
                    {
                        "nodeId": "server-node",
                        "endpointId": "server-node-service-tcp",
                    }
                ],
            )
            self.assertEqual(client["server_endpoints"], [])
            self.assertEqual(server["pduSize"]["server"], {
                "heapSize": 128,
                "baseSize": 296,
            })
            self.assertEqual(server["pduSize"]["client"], {
                "heapSize": 64,
                "baseSize": 288,
            })
            self.assertEqual(
                [
                    (
                        item["name"],
                        item["requestChannelId"],
                        item["responseChannelId"],
                    )
                    for item in server["clients"]
                ],
                [
                    ("hakoniwa_pdu_ros_add_0", 0, 1),
                    ("hakoniwa_pdu_ros_add_1", 2, 3),
                ],
            )

            server_transport = json.loads(
                (output / "transport" / "server-node.json").read_text()
            )
            self.assertNotIn("role", server_transport)
            self.assertEqual(server_transport["expected_clients"], 2)
            self.assertEqual(server_transport["local"]["port"], 54010)
            client_transport = json.loads(
                (output / "transport" / "ros-service.json").read_text()
            )
            self.assertEqual(client_transport["role"], "client")
            self.assertEqual(client_transport["remote"]["port"], 54010)

    def test_expected_clients_are_summed_for_shared_server(self) -> None:
        source = manifest()
        second = json.loads(json.dumps(source["services"][0]))
        second["name"] = "Service/Subtract"
        second["type"] = "hako_srv_msgs/SubtractTwoInts"
        second["clientNamePrefix"] = "hakoniwa_pdu_ros_subtract"
        second["maxClients"] = 3
        source["services"].append(second)

        resolved = GENERATOR.resolve(source)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "service.json"
            config.write_text(json.dumps(source), encoding="utf-8")
            GENERATOR.generate(config, root / "generated")
            transport = json.loads(
                (root / "generated" / "transport" / "server-node.json").read_text()
            )
        self.assertEqual(len(resolved["services"]), 2)
        self.assertEqual(transport["expected_clients"], 5)

    def test_rejects_wrong_transport_roles(self) -> None:
        invalid = manifest()
        invalid["transport"]["endpoints"]["server-node"] = {
            "role": "client",
            "remote": {"address": "127.0.0.1", "port": 54010},
        }
        with self.assertRaisesRegex(GENERATOR.ConfigurationError, "serverEndpoint"):
            GENERATOR.resolve(invalid)

    def test_rejects_missing_endpoint_and_unknown_fields(self) -> None:
        missing = manifest()
        del missing["transport"]["endpoints"]["server-node"]
        with self.assertRaisesRegex(GENERATOR.ConfigurationError, "missing referenced"):
            GENERATOR.resolve(missing)

        unknown = manifest()
        unknown["services"][0]["rpc_client_names"] = []
        with self.assertRaisesRegex(GENERATOR.ConfigurationError, "unknown fields"):
            GENERATOR.resolve(unknown)

    def test_generation_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "service.json"
            output = root / "generated"
            source.write_text(json.dumps(manifest()), encoding="utf-8")
            GENERATOR.generate(source, output)
            first = {
                path.relative_to(output): path.read_bytes()
                for path in output.rglob("*.json")
            }
            GENERATOR.generate(source, output)
            second = {
                path.relative_to(output): path.read_bytes()
                for path in output.rglob("*.json")
            }
            self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
