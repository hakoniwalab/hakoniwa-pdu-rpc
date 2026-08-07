import importlib.util
import json
import io
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("generate_action_config.py")
SPEC = importlib.util.spec_from_file_location("generate_action_config", MODULE_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(GENERATOR)


def manifest(client_role="client", server_role="server"):
    endpoint_by_role = {
        "client": {"role": "client", "remote": {"address": "127.0.0.1", "port": 54011}},
        "server": {"role": "server", "local": {"address": "0.0.0.0", "port": 54011}},
    }
    return {
        "version": 1,
        "actions": [
            {
                "name": "fibonacci",
                "type": "sample_action_msgs/Fibonacci",
                "slotCount": 2,
                "bufferHeap": {
                    "requestSize": 4096,
                    "responseSize": 8192,
                    "feedbackSize": 2048,
                },
                "clientEndpoint": {"nodeId": "fibonacci-client"},
                "serverEndpoint": {"nodeId": "fibonacci-server"},
            }
        ],
        "transport": {
            "protocol": "tcp",
            "packetVersion": "v2",
            "endpoints": {
                "fibonacci-client": endpoint_by_role[client_role],
                "fibonacci-server": endpoint_by_role[server_role],
            },
        },
    }


class GenerateActionConfigTest(unittest.TestCase):
    def test_generates_visible_resolved_and_endpoint_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "action.json"
            output = root / "generated"
            source.write_text(json.dumps(manifest()), encoding="utf-8")

            GENERATOR.generate(source, output)

            resolved = json.loads((output / "resolved-action.json").read_text())
            action = resolved["actions"][0]
            self.assertEqual(action["clientEndpoint"]["endpointId"], "fibonacci-client-action-tcp")
            self.assertEqual(action["serverEndpoint"]["endpointId"], "fibonacci-server-action-tcp")
            self.assertEqual(len(action["channels"]), 6)
            self.assertEqual(action["channels"][5]["channelId"], 5)
            self.assertEqual(action["channels"][5]["channelName"], "Slot1Feedback")
            self.assertEqual(action["bufferHeap"]["requestSize"], 4096)
            self.assertEqual(action["bufferHeap"]["responseSize"], 8192)
            self.assertEqual(action["bufferHeap"]["feedbackSize"], 2048)

            client_tcp = json.loads(
                (output / "transport" / "fibonacci-client.json").read_text()
            )
            server_tcp = json.loads(
                (output / "transport" / "fibonacci-server.json").read_text()
            )
            self.assertEqual(client_tcp["role"], "client")
            self.assertEqual(client_tcp["remote"]["port"], 54011)
            self.assertEqual(server_tcp["role"], "server")
            self.assertEqual(server_tcp["local"]["port"], 54011)
            self.assertNotIn("pdu_def_path", json.loads(
                (output / "endpoints" / "fibonacci-client.json").read_text()
            ))

    def test_action_client_may_be_tcp_server(self):
        reversed_manifest = manifest(client_role="server", server_role="client")
        resolved = GENERATOR.resolve(reversed_manifest)
        self.assertEqual(
            resolved["transport"]["endpoints"]["fibonacci-client"]["role"],
            "server",
        )
        self.assertEqual(
            resolved["transport"]["endpoints"]["fibonacci-server"]["role"],
            "client",
        )

    def test_omitted_buffer_heap_warns_and_resolves_safe_defaults(self):
        source = manifest()
        del source["actions"][0]["bufferHeap"]
        warning = io.StringIO()
        with redirect_stderr(warning):
            resolved = GENERATOR.resolve(source)

        self.assertIn("bufferHeap is not configured", warning.getvalue())
        self.assertEqual(
            resolved["actions"][0]["bufferHeap"],
            {
                "requestSize": 1048576,
                "responseSize": 1048576,
                "feedbackSize": 1048576,
            },
        )

    def test_rejects_invalid_buffer_heap(self):
        invalid = manifest()
        invalid["actions"][0]["bufferHeap"]["feedbackSize"] = -1
        with self.assertRaisesRegex(GENERATOR.ConfigurationError, "feedbackSize"):
            GENERATOR.resolve(invalid)

    def test_partial_buffer_heap_warns_and_resolves_missing_values(self):
        source = manifest()
        source["actions"][0]["bufferHeap"] = {"requestSize": 4096}
        warning = io.StringIO()
        with redirect_stderr(warning):
            resolved = GENERATOR.resolve(source)

        self.assertIn("omits responseSize, feedbackSize", warning.getvalue())
        self.assertEqual(resolved["actions"][0]["bufferHeap"]["requestSize"], 4096)
        self.assertEqual(resolved["actions"][0]["bufferHeap"]["responseSize"], 1048576)
        self.assertEqual(resolved["actions"][0]["bufferHeap"]["feedbackSize"], 1048576)

    def test_rejects_missing_referenced_endpoint(self):
        invalid = manifest()
        del invalid["transport"]["endpoints"]["fibonacci-server"]
        with self.assertRaisesRegex(GENERATOR.ConfigurationError, "missing referenced nodeIds"):
            GENERATOR.resolve(invalid)

    def test_rejects_role_specific_address_mismatch(self):
        invalid = manifest()
        invalid["transport"]["endpoints"]["fibonacci-client"] = {
            "role": "client",
            "local": {"address": "127.0.0.1", "port": 54011},
        }
        with self.assertRaisesRegex(GENERATOR.ConfigurationError, "local is not valid"):
            GENERATOR.resolve(invalid)

    def test_generation_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "action.json"
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
