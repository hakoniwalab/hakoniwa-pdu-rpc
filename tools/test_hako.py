from __future__ import annotations

import argparse
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
