from __future__ import annotations

import argparse
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


MODULE_PATH = Path(__file__).with_name("hako.py")
SPEC = importlib.util.spec_from_file_location("hako_build_tool", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
HAKO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HAKO)
REPO_ROOT = MODULE_PATH.resolve().parents[1]


def write_endpoint_package(root: Path) -> None:
    config = (
        root
        / "lib"
        / "cmake"
        / "hakoniwa_pdu_endpoint"
        / "hakoniwa_pdu_endpointConfig.cmake"
    )
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text("# test package\n", encoding="utf-8")


def make_args(**overrides):
    values = {
        "build_dir": None,
        "install_dir": None,
        "build_type": None,
        "endpoint_root": None,
        "vcpkg_root": None,
        "python_venv": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class ManifestTests(unittest.TestCase):
    def load(self, text: str):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = Path(temp_dir) / "build.yaml"
            manifest.write_text(text, encoding="utf-8")
            return HAKO.resolve_config(HAKO.load_simple_yaml(manifest))

    def test_default_manifest_reproduces_legacy_build_intent(self):
        cfg = HAKO.resolve_config(
            HAKO.load_simple_yaml(REPO_ROOT / "hakoniwa-build.yaml")
        )
        self.assertEqual(
            cfg,
            {
                "version": 1,
                "build": {
                    "type": "Release",
                    "dir": "build",
                    "install_dir": ".hako/install",
                },
                "paths": {
                    "pdu_endpoint_root": "",
                    "vcpkg_root": "",
                },
            },
        )

    def test_alternate_manifest_changes_only_configurable_values(self):
        cfg = HAKO.resolve_config(
            HAKO.load_simple_yaml(
                REPO_ROOT / "test" / "fixtures" / "alternate-build.yaml"
            )
        )
        self.assertEqual(cfg["build"]["type"], "Debug")
        self.assertEqual(cfg["build"]["dir"], "build-alternate")
        self.assertEqual(cfg["build"]["install_dir"], ".hako/install-alternate")
        self.assertEqual(cfg["paths"]["pdu_endpoint_root"], "")

    def test_unknown_key_is_rejected(self):
        with self.assertRaisesRegex(HAKO.ConfigError, "unknown key"):
            self.load(
                """version: 1
build:
  type: Release
  dir: build
  install_dir: .hako/install
  parallel: 8
paths:
  pdu_endpoint_root: ""
  vcpkg_root: ""
"""
            )

    def test_missing_required_key_is_rejected(self):
        with self.assertRaisesRegex(HAKO.ConfigError, "missing required key"):
            self.load(
                """version: 1
build:
  type: Release
  dir: build
paths:
  pdu_endpoint_root: ""
  vcpkg_root: ""
"""
            )

    def test_invalid_type_is_rejected(self):
        with self.assertRaisesRegex(HAKO.ConfigError, "must be a non-empty string"):
            self.load(
                """version: 1
build:
  type: Release
  dir: 123
  install_dir: .hako/install
paths:
  pdu_endpoint_root: ""
  vcpkg_root: ""
"""
            )

    def test_default_manifest_is_repo_relative_but_explicit_path_is_cwd_relative(self):
        parser = HAKO.create_parser()
        args = parser.parse_args(["doctor"])
        self.assertIsNone(args.config)
        self.assertEqual(
            HAKO.resolve_manifest_path(args.config, REPO_ROOT),
            REPO_ROOT / "hakoniwa-build.yaml",
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            work = Path(temp_dir)
            explicit = work / "custom.yaml"
            explicit.write_text("version: 1\n", encoding="utf-8")
            original = Path.cwd()
            try:
                os.chdir(work)
                self.assertEqual(
                    HAKO.resolve_manifest_path("custom.yaml", REPO_ROOT),
                    explicit.resolve(),
                )
            finally:
                os.chdir(original)


class PrecedenceTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.manifest = self.root / "hakoniwa-build.yaml"

    def tearDown(self):
        self.temp_dir.cleanup()

    def config(self, endpoint_root: str = "", vcpkg_root: str = ""):
        return {
            "version": 1,
            "build": {
                "type": "Release",
                "dir": "build",
                "install_dir": ".hako/install",
            },
            "paths": {
                "pdu_endpoint_root": endpoint_root,
                "vcpkg_root": vcpkg_root,
            },
        }

    def test_cli_overrides_manifest_and_environment(self):
        cli = self.root / "cli-endpoint"
        manifest = self.root / "manifest-endpoint"
        environment = self.root / "environment-endpoint"
        for path in (cli, manifest, environment):
            write_endpoint_package(path)
        with patch.dict(
            HAKO.os.environ,
            {"HAKO_PDU_ENDPOINT_ROOT": str(environment)},
            clear=False,
        ):
            ctx = HAKO.Context(
                make_args(endpoint_root=str(cli)),
                self.config(endpoint_root=str(manifest)),
                self.manifest,
                self.root,
            )
        self.assertEqual(ctx.endpoint_root, cli.resolve())
        self.assertEqual(ctx.endpoint_source, "cli")

    def test_manifest_overrides_environment(self):
        manifest = self.root / "manifest-endpoint"
        environment = self.root / "environment-endpoint"
        for path in (manifest, environment):
            write_endpoint_package(path)
        with patch.dict(
            HAKO.os.environ,
            {"HAKO_PDU_ENDPOINT_ROOT": str(environment)},
            clear=False,
        ):
            ctx = HAKO.Context(
                make_args(),
                self.config(endpoint_root=str(manifest)),
                self.manifest,
                self.root,
            )
        self.assertEqual(ctx.endpoint_root, manifest.resolve())
        self.assertEqual(ctx.endpoint_source, "manifest")

    def test_environment_is_used_when_manifest_path_is_empty(self):
        environment = self.root / "environment-endpoint"
        write_endpoint_package(environment)
        with patch.dict(
            HAKO.os.environ,
            {"HAKO_PDU_ENDPOINT_ROOT": str(environment)},
            clear=False,
        ):
            ctx = HAKO.Context(
                make_args(),
                self.config(),
                self.manifest,
                self.root,
            )
        self.assertEqual(ctx.endpoint_root, environment.resolve())
        self.assertEqual(
            ctx.endpoint_source,
            "environment:HAKO_PDU_ENDPOINT_ROOT",
        )

    def test_environment_discovery_skips_invalid_legacy_candidate(self):
        valid = self.root / "valid-environment-endpoint"
        write_endpoint_package(valid)
        with patch.dict(
            HAKO.os.environ,
            {
                "HAKO_PDU_ENDPOINT_ROOT": str(self.root / "missing-endpoint"),
                "HAKO_PDU_ENDPOINT_PREFIX": str(valid),
            },
            clear=False,
        ):
            ctx = HAKO.Context(
                make_args(),
                self.config(),
                self.manifest,
                self.root,
            )
        self.assertEqual(ctx.endpoint_root, valid.resolve())
        self.assertEqual(
            ctx.endpoint_source,
            "environment:HAKO_PDU_ENDPOINT_PREFIX",
        )

    def test_invalid_manifest_path_is_not_replaced_by_environment(self):
        selected = self.root / "missing-endpoint"
        environment = self.root / "environment-endpoint"
        write_endpoint_package(environment)
        with patch.dict(
            HAKO.os.environ,
            {"HAKO_PDU_ENDPOINT_ROOT": str(environment)},
            clear=False,
        ):
            ctx = HAKO.Context(
                make_args(),
                self.config(endpoint_root=str(selected)),
                self.manifest,
                self.root,
            )
        self.assertEqual(ctx.endpoint_root, selected.resolve())
        self.assertEqual(ctx.endpoint_source, "manifest")

    def test_cli_build_values_override_manifest(self):
        ctx = HAKO.Context(
            make_args(
                build_dir="cli-build",
                install_dir="cli-install",
                build_type="RelWithDebInfo",
            ),
            self.config(),
            self.manifest,
            self.root,
        )
        self.assertEqual(ctx.build_dir, (self.root / "cli-build").resolve())
        self.assertEqual(ctx.install_dir, (self.root / "cli-install").resolve())
        self.assertEqual(ctx.build_type, "RelWithDebInfo")


class OperationCompatibilityTests(unittest.TestCase):
    def context(self):
        cfg = HAKO.resolve_config(
            HAKO.load_simple_yaml(REPO_ROOT / "hakoniwa-build.yaml")
        )
        return HAKO.Context(
            make_args(),
            cfg,
            REPO_ROOT / "hakoniwa-build.yaml",
            REPO_ROOT,
        )

    def test_default_build_cmake_contract_matches_legacy_driver(self):
        ctx = self.context()
        command = HAKO.configure_command(ctx, tests=False)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", command)
        self.assertIn("-DHAKO_PDU_RPC_BUILD_TESTS=OFF", command)
        self.assertIn("-DHAKO_PDU_RPC_BUILD_EXAMPLES=ON", command)
        self.assertEqual(ctx.build_dir, REPO_ROOT / "build")
        self.assertEqual(ctx.install_dir, REPO_ROOT / ".hako" / "install")

    def test_test_operations_keep_tests_enabled(self):
        ctx = self.context()
        for operation in HAKO.TEST_COMMANDS:
            with self.subTest(operation=operation):
                record = HAKO.resolved_record(ctx, operation)
                self.assertTrue(record["operation_semantics"]["tests"])
                self.assertIn(
                    "-DHAKO_PDU_RPC_BUILD_TESTS=ON",
                    record["cmake_args"],
                )

    def test_reviewed_test_suite_includes_action_contracts(self):
        expected = {
            "hakoniwa_pdu_action_configuration_test",
            "hakoniwa_pdu_action_server_state_machine_test",
            "hakoniwa_pdu_action_services_server_goal_instance_test",
            "hakoniwa_pdu_action_client_state_machine_test",
            "hakoniwa_pdu_action_services_client_goal_instance_test",
            "hakoniwa_pdu_action_server_initialization_test",
            "hakoniwa_pdu_action_goal_response_transaction_test",
            "hakoniwa_pdu_action_cancel_response_serialization_test",
            "hakoniwa_pdu_action_client_endpoint_test",
            "hakoniwa_pdu_action_packet_codec_test",
            "hakoniwa_pdu_action_tcp_e2e_test",
            "hakoniwa_pdu_action_mux_server_test",
            "hakoniwa_pdu_action_c_api_mux_server_test",
            "hakoniwa_pdu_action_c_api_header_test",
            "hakoniwa_pdu_action_c_api_tcp_e2e_test",
        }
        self.assertEqual(set(HAKO.ACTION_CONTRACT_TARGETS), expected)
        self.assertTrue(expected.issubset(set(HAKO.REVIEWED_TEST_TARGETS)))
        self.assertEqual(
            HAKO.REVIEWED_TEST_BUILD_TARGET,
            "hakoniwa_pdu_rpc_reviewed_tests",
        )
        for target in expected:
            self.assertRegex(target, HAKO.REVIEWED_TEST_REGEX)

    def test_non_test_operations_keep_tests_disabled(self):
        ctx = self.context()
        for operation in (
            "doctor",
            "configure",
            "build",
            "install",
            "package-test",
        ):
            with self.subTest(operation=operation):
                record = HAKO.resolved_record(ctx, operation)
                self.assertFalse(record["operation_semantics"]["tests"])
                self.assertIn(
                    "-DHAKO_PDU_RPC_BUILD_TESTS=OFF",
                    record["cmake_args"],
                )


class FoundationInstallTests(unittest.TestCase):
    def test_python_install_resolves_declared_dependencies(self):
        context = SimpleNamespace(
            venv_python=Path("/foundation/python/bin/python"),
            repo_root=Path("/src/hakoniwa-pdu-rpc"),
        )
        with patch.object(HAKO, "run") as run_command:
            HAKO.install_python_package(context)

        command = run_command.call_args.args[0]
        self.assertIn("--force-reinstall", command)
        self.assertNotIn("--no-deps", command)
        self.assertNotIn("--no-build-isolation", command)

    def test_python_venv_is_optional_and_resolved_under_cli_control(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            endpoint = root / "endpoint"
            write_endpoint_package(endpoint)
            venv = root / "install" / "python"
            interpreter = (
                venv / "Scripts" / "python.exe"
                if HAKO.host_platform()[0] == "windows"
                else venv / "bin" / "python"
            )
            interpreter.parent.mkdir(parents=True)
            interpreter.write_text("test\n", encoding="utf-8")
            (root / "hakoniwa-pdu-registry" / "pdu" / "types").mkdir(
                parents=True
            )
            ctx = HAKO.Context(
                make_args(
                    endpoint_root=str(endpoint),
                    python_venv=str(venv),
                ),
                {
                    "version": 1,
                    "build": {
                        "type": "Release",
                        "dir": "build",
                        "install_dir": "install",
                    },
                    "paths": {
                        "pdu_endpoint_root": "",
                        "vcpkg_root": "",
                    },
                },
                root / "hakoniwa-build.yaml",
                root,
            )

            self.assertEqual(ctx.python_venv, venv.resolve())
            self.assertEqual(ctx.venv_python, interpreter.resolve())
            self.assertEqual(HAKO.doctor(ctx), [])
            self.assertEqual(
                HAKO.resolved_record(ctx, "install")["paths"]["python_venv"],
                str(venv.resolve()),
            )

    def test_dependency_receipt_reads_endpoint_contract_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            prefix = Path(temp_dir)
            receipt = (
                prefix
                / "share"
                / "hakoniwa"
                / "receipts"
                / "hakoniwa-pdu-endpoint.yaml"
            )
            receipt.parent.mkdir(parents=True)
            receipt.write_text(
                """schema_version: 1
component:
  id: hakoniwa-pdu-endpoint
  version: 1.0.0
  source_revision: "def456"
build_limits:
  asset_num: 16
artifacts:
  - path: "lib/libhakoniwa_pdu_endpoint.so"
    kind: library
""",
                encoding="utf-8",
            )

            dependency = HAKO._read_dependency_receipt(
                prefix,
                "hakoniwa-pdu-endpoint",
            )

            self.assertEqual(dependency["version"], "1.0.0")
            self.assertEqual(dependency["source_revision"], "def456")
            self.assertEqual(dependency["build_limits"]["asset_num"], 16)

    def test_missing_legacy_dependency_receipt_is_unknown(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            dependency = HAKO._read_dependency_receipt(
                Path(temp_dir),
                "hakoniwa-pdu-endpoint",
            )
        self.assertEqual(dependency["source_revision"], "unknown")
        self.assertEqual(dependency["build_limits"], {})

    def test_rpc_artifacts_are_compact_install_surfaces(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            prefix = Path(temp_dir)
            headers = prefix / "include" / "hakoniwa" / "pdu" / "rpc"
            headers.mkdir(parents=True)
            for index in range(100):
                (headers / f"rpc_{index}.hpp").write_text("", encoding="utf-8")
            cmake_dir = prefix / "lib" / "cmake" / "hakoniwa_pdu_rpc"
            cmake_dir.mkdir(parents=True)
            (prefix / "lib" / "libhakoniwa_pdu_rpc.a").write_text(
                "",
                encoding="utf-8",
            )

            artifacts = HAKO._rpc_artifacts(prefix)

            self.assertIn(
                (Path("include/hakoniwa/pdu/rpc"), "directory"),
                artifacts,
            )
            self.assertLess(len(artifacts), 10)

    def test_python_artifacts_include_schema_below_selected_venv(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            prefix = Path(temp_dir)
            venv = prefix / "python"
            package = venv / "lib" / "site-packages" / "hakoniwa_pdu_rpc"
            schema = venv / "share" / "hakoniwa-pdu-rpc" / "schema"
            package.mkdir(parents=True)
            schema.mkdir(parents=True)
            context = SimpleNamespace(
                install_dir=prefix.resolve(),
                python_venv=venv.resolve(),
                venv_python=venv / "bin" / "python",
            )

            with patch.object(
                HAKO.subprocess,
                "run",
                return_value=SimpleNamespace(stdout=f"{package}\n"),
            ):
                artifacts = HAKO._python_package_artifacts(context)

            self.assertIn(
                (Path("python/lib/site-packages/hakoniwa_pdu_rpc"), "python-package"),
                artifacts,
            )
            self.assertIn(
                (
                    Path("python/share/hakoniwa-pdu-rpc/schema"),
                    "schema-directory",
                ),
                artifacts,
            )


if __name__ == "__main__":
    unittest.main()
